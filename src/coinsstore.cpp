// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <logging.h>
#include <memusage.h>
#include <util/check.h>
#include <util/threadnames.h>

#include <algorithm>
#include <array>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
//! What the calling thread is doing with the store.
thread_local CoinsStore::Site t_site{CoinsStore::Site::OTHER};
struct SiteScope {
    const CoinsStore::Site prev{t_site};
    explicit SiteScope(CoinsStore::Site site) { t_site = site; }
    ~SiteScope() { t_site = prev; }
};
struct alignas(64) WaitCounter {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> slept{0};
    std::atomic<int64_t> ns{0};
};
//! By waiter, then holder.
std::array<std::array<WaitCounter, CoinsStore::NUM_SITES>, CoinsStore::NUM_SITES> g_lock_waits;
//! How long a waiter retries before it blocks: about what blocking and being woken would cost it.
constexpr std::chrono::microseconds SHARD_RETRY_TIME{20};
} // namespace

void CoinsStore::ShardLock::lock()
{
    if (!m_mutex.try_lock()) {
        const auto holder{m_holder.load(std::memory_order_relaxed)};
        const auto start{std::chrono::steady_clock::now()};
        bool locked{false};
        do {
            std::this_thread::yield();
            locked = m_mutex.try_lock();
        } while (!locked && std::chrono::steady_clock::now() - start < SHARD_RETRY_TIME);
        WaitCounter& counter{g_lock_waits[static_cast<size_t>(t_site)][holder]};
        if (!locked) {
            m_mutex.lock();
            counter.slept.fetch_add(1, std::memory_order_relaxed);
        }
        counter.count.fetch_add(1, std::memory_order_relaxed);
        counter.ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(), std::memory_order_relaxed);
    }
    m_holder.store(static_cast<uint8_t>(t_site), std::memory_order_relaxed);
}

bool CoinsStore::ShardLock::try_lock()
{
    if (!m_mutex.try_lock()) return false;
    m_holder.store(static_cast<uint8_t>(t_site), std::memory_order_relaxed);
    return true;
}

const char* CoinsStore::SiteName(Site site)
{
    switch (site) {
    case Site::OTHER: return "other";
    case Site::LOOKUP: return "lookup";
    case Site::FETCH: return "fetch";
    case Site::APPLY: return "apply";
    case Site::SWEEP: return "sweep";
    case Site::WRITE_OUT: return "writeout";
    }
    return "?";
}

std::vector<CoinsStore::LockWait> CoinsStore::LockWaits()
{
    std::vector<LockWait> waits;
    for (size_t waiter{0}; waiter < NUM_SITES; ++waiter) {
        for (size_t holder{0}; holder < NUM_SITES; ++holder) {
            const WaitCounter& counter{g_lock_waits[waiter][holder]};
            const uint64_t count{counter.count.load(std::memory_order_relaxed)};
            if (count == 0) continue;
            waits.push_back({static_cast<Site>(waiter), static_cast<Site>(holder), count, counter.slept.load(std::memory_order_relaxed),
                             std::chrono::nanoseconds{counter.ns.load(std::memory_order_relaxed)}});
        }
    }
    return waits;
}

CoinsStore::CoinsStore(CCoinsView* base_view, size_t shards, bool deterministic)
    : CCoinsViewCache(base_view, deterministic), m_shards(std::max<size_t>(1, shards))
{
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        Resized(shard); // an empty map has a bucket already
    }
}

CoinsStore::~CoinsStore() = default;

// ---------------------------------------------------------------------------------------------------------
// Entry bookkeeping

CoinsStore::Totals& CoinsStore::MyTotals() const
{
    // Stripes are dealt in turn; threads beyond their number share, which costs a shared line and nothing else.
    static std::atomic<size_t> next{0};
    static thread_local const size_t stripe{next.fetch_add(1, std::memory_order_relaxed) % TOTALS_STRIPES};
    return m_totals[stripe];
}

size_t CoinsStore::SumTotals(std::atomic<int64_t> Totals::* field) const
{
    int64_t total{0};
    for (const Totals& totals : m_totals) total += (totals.*field).load(std::memory_order_relaxed);
    // Read beside a change under way, an addition can be missed whose removal is seen.
    return static_cast<size_t>(std::max<int64_t>(0, total));
}

int64_t CoinsStore::NodeBytes()
{
    return static_cast<int64_t>(memusage::MallocUsage(sizeof(memusage::unordered_node<Map::value_type>)));
}

size_t CoinsStore::SpanOf(int height)
{
    return static_cast<size_t>(std::max(height, 0)) >> DIRTY_SPAN_BITS;
}

CoinsStore::Bucket& CoinsStore::DirtyBucket(Shard& shard, int height) const
{
    AssertLockHeld(shard.mutex);
    const size_t span{SpanOf(height)};
    if (span >= shard.dirty_spans.size()) {
        const auto before{static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans))};
        shard.dirty_spans.resize(span + 1);
        MyTotals().bytes.fetch_add(static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans)) - before, std::memory_order_relaxed);
    }
    return shard.dirty_spans[span];
}

void CoinsStore::FreeDirtySpans(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans)), std::memory_order_relaxed);
    std::vector<Bucket>{}.swap(shard.dirty_spans);
}

void CoinsStore::AddUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    shard.coins_usage += bytes;
    MyTotals().bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::SubUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    Assume(shard.coins_usage >= bytes);
    shard.coins_usage -= bytes;
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::Resized(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    // As memusage::DynamicUsage(shard.map) computes it: a node for each entry and the bucket array.
    const size_t entries{shard.map.size()};
    const size_t buckets{shard.map.bucket_count()};
    if (entries == shard.entries && buckets == shard.buckets) return;
    const int64_t delta{static_cast<int64_t>(entries) - static_cast<int64_t>(shard.entries)};
    int64_t bytes{NodeBytes() * delta};
    if (buckets != shard.buckets) {
        bytes += static_cast<int64_t>(memusage::MallocUsage(sizeof(void*) * buckets)) - static_cast<int64_t>(memusage::MallocUsage(sizeof(void*) * shard.buckets));
    }
    Totals& totals{MyTotals()};
    totals.entries.fetch_add(delta, std::memory_order_relaxed);
    totals.bytes.fetch_add(bytes, std::memory_order_relaxed);
    shard.entries = entries;
    shard.buckets = buckets;
}

int64_t CoinsStore::DirtyBytes(const Entry& entry)
{
    // The node and the coin's script, as DynamicMemoryUsage() counts them: what the tier's usage falls by
    // when the entry is written out and then evicted (or, for a spent one, erased).
    return static_cast<int64_t>(memusage::MallocUsage(sizeof(memusage::unordered_node<Map::value_type>)) + entry.coin.DynamicMemoryUsage());
}

void CoinsStore::Detach(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    if (entry.list == LIST_DIRTY) {
        Bucket& bucket{shard.dirty_spans[SpanOf(entry.height)]};
        if (entry.prev) entry.prev->second.next = entry.next; else bucket.head = entry.next;
        if (entry.next) entry.next->second.prev = entry.prev;
        bucket.bytes -= DirtyBytes(entry);
        Assume(bucket.head || bucket.bytes == 0);
        Assume(shard.dirty_count > 0);
        --shard.dirty_count;
        MyTotals().dirty.fetch_sub(1, std::memory_order_relaxed);
        if (shard.dirty_count == 0) FreeDirtySpans(shard);
    } else if (entry.list == LIST_CLEAN) {
        // prev is towards the young end, next towards the old end.
        if (entry.prev) entry.prev->second.next = entry.next; else shard.clean_young = entry.next;
        if (entry.next) entry.next->second.prev = entry.prev; else shard.clean_old = entry.prev;
    }
    entry.list = LIST_NONE;
    entry.prev = nullptr;
    entry.next = nullptr;
}

void CoinsStore::Attach(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    Assume(entry.list == LIST_NONE);
    if (entry.dirty) {
        Bucket& bucket{DirtyBucket(shard, entry.height)};
        entry.prev = nullptr;
        entry.next = bucket.head;
        if (bucket.head) bucket.head->second.prev = &node;
        bucket.head = &node;
        bucket.bytes += DirtyBytes(entry);
        ++shard.dirty_count;
        MyTotals().dirty.fetch_add(1, std::memory_order_relaxed);
        entry.list = LIST_DIRTY;
    } else {
        entry.prev = nullptr;
        entry.next = shard.clean_young;
        if (shard.clean_young) shard.clean_young->second.prev = &node; else shard.clean_old = &node;
        shard.clean_young = &node;
        entry.list = LIST_CLEAN;
    }
}

void CoinsStore::EraseDetached(Shard& shard, Map::iterator it) const
{
    AssertLockHeld(shard.mutex);
    Assume(it->second.list == LIST_NONE);
    // A run holds a pointer to every entry it collected until their chunk settles.
    Assume(!it->second.collected);
    shard.map.erase(it);
    Resized(shard);
}

void CoinsStore::EraseEntry(Shard& shard, Map::iterator it) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{it->second};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    EraseDetached(shard, it);
}

void CoinsStore::EraseEntry(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    Detach(shard, node);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    // A run holds a pointer to every entry it collected until their chunk settles.
    Assume(!entry.collected);
    // A copy of the key, which goes with the node.
    const COutPoint outpoint{node.first};
    shard.map.erase(outpoint);
    Resized(shard);
}

// ---------------------------------------------------------------------------------------------------------
// Lookups

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    const SiteScope site{Site::LOOKUP};
    Shard& shard{ShardFor(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) {
                if (it->second.coin.IsSpent()) return std::nullopt;
                Reference(it->second);
                return it->second.coin;
            }
            settles = shard.settles;
        }
        // A miss: read the base with the shard unlocked, then re-take the shard and insert unless another
        // thread brought the entry in meanwhile (in which case its state wins, whatever it is).
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        // A write-out settled entries of the shard meanwhile: the coin may have been spent, written as an
        // erase and dropped from the tier since the miss, with the base read before the erase. Look again.
        if (shard.settles != settles) continue;
        if (!coin) return std::nullopt;
        Assert(!coin->IsSpent());
        auto [it, inserted]{shard.map.try_emplace(outpoint)};
        if (inserted) {
            Resized(shard);
            it->second.coin = *coin;
            it->second.height = coin->nHeight;
            AddUsage(shard, it->second.coin.DynamicMemoryUsage());
            Attach(shard, *it);
            return coin;
        }
        if (it->second.coin.IsSpent()) return std::nullopt;
        return it->second.coin;
    }
}

std::optional<Coin> CoinsStore::PeekCoin(const COutPoint& outpoint) const
{
    const SiteScope site{Site::LOOKUP};
    Shard& shard{ShardFor(outpoint)};
    {
        LOCK(shard.mutex);
        if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) {
            if (it->second.coin.IsSpent()) return std::nullopt;
            Reference(it->second);
            return it->second.coin;
        }
    }
    return base->PeekCoin(outpoint);
}

std::optional<Coin> CoinsStore::GetCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint);
}

bool CoinsStore::HaveCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint).has_value();
}

bool CoinsStore::HaveCoinInCache(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const auto it{shard.map.find(outpoint)};
    return it != shard.map.end() && !it->second.coin.IsSpent();
}

static const Coin STORE_EMPTY_COIN;

const Coin& CoinsStore::AccessCoin(const COutPoint& outpoint) const
{
    // Bring the entry in if needed, then hand out a reference to it. Valid while the owner's lock keeps the
    // entry from being erased or replaced.
    if (!Lookup(outpoint)) return STORE_EMPTY_COIN;
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const auto it{shard.map.find(outpoint)};
    if (it == shard.map.end() || it->second.coin.IsSpent()) return STORE_EMPTY_COIN;
    Reference(it->second);
    return it->second.coin;
}

uint256 CoinsStore::GetBestBlock() const
{
    if (m_tier_block_hash.IsNull()) m_tier_block_hash = base->GetBestBlock();
    return m_tier_block_hash;
}

void CoinsStore::SetBestBlock(const uint256& block_hash)
{
    m_tier_block_hash = block_hash;
}

void CoinsStore::SetBestBlock(const uint256& block_hash, int height)
{
    m_tier_block_hash = block_hash;
    m_change_height = height;
    m_tier_height = height;
}

// ---------------------------------------------------------------------------------------------------------
// Changes

void CoinsStore::AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    Entry& entry{it->second};
    bool fresh{false};
    if (!possible_overwrite) {
        if (!entry.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // A spent entry that is dirty records a spentness not yet in the database; re-adding the coin must
        // not make it fresh, or a later spend would drop the entry and the database would keep the coin.
        // A new entry, or a spent entry already written, can be fresh. (See CCoinsViewCache::AddCoin.)
        fresh = !entry.dirty;
    }
    if (!inserted) {
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
    }
    entry.coin = std::move(coin);
    entry.height = TagFor(entry.coin);
    entry.fresh = fresh;
    Touch(entry);
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    entry.dirty = true;
    Attach(shard, *it);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const size_t usage{coin.DynamicMemoryUsage()};
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    if (!inserted) return;
    Entry& entry{it->second};
    entry.coin = std::move(coin);
    entry.height = m_change_height;
    entry.fresh = false;
    Touch(entry);
    AddUsage(shard, usage);
    entry.dirty = true;
    Attach(shard, *it);
}

bool CoinsStore::SpendEntry(Shard& shard, Map::iterator it, Coin* moveto)
{
    AssertLockHeld(shard.mutex);
    // As CCoinsViewCache::SpendCoin: an entry that is present is "spent" whether or not it still holds a
    // coin, so that a caller spending an already spent entry gets the same answer from both.
    Entry& entry{it->second};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    if (moveto) *moveto = std::move(entry.coin);
    if (entry.fresh) {
        // Never written: the database has nothing to erase.
        EraseDetached(shard, it);
        return true;
    }
    entry.coin.Clear();
    entry.height = m_change_height;
    Touch(entry);
    entry.dirty = true;
    Attach(shard, *it);
    return true;
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard{ShardFor(outpoint)};
    {
        LOCK(shard.mutex);
        if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) return SpendEntry(shard, it, moveto);
    }
    // Not in the tier: bring the coin in from the base, then spend it; nothing to spend if the base lacks it.
    if (!Lookup(outpoint)) return false;
    LOCK(shard.mutex);
    auto it{shard.map.find(outpoint)};
    if (it == shard.map.end()) return false;
    return SpendEntry(shard, it, moveto);
}

void CoinsStore::Uncache(const COutPoint& outpoint)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto it{shard.map.find(outpoint)};
    if (it == shard.map.end() || it->second.dirty) return;
    EraseEntry(shard, it);
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) continue;
        const COutPoint& outpoint{it->first};
        Shard& shard{ShardFor(outpoint)};
        LOCK(shard.mutex);
        auto [ours, inserted]{shard.map.try_emplace(outpoint)};
        if (inserted) Resized(shard);
        Entry& entry{ours->second};
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                // Created and spent inside the child: nothing to record.
                EraseDetached(shard, ours);
                continue;
            }
            entry.coin = cursor.WillErase(*it) ? std::move(it->second.coin) : it->second.coin;
            entry.fresh = it->second.IsFresh();
        } else {
            if (it->second.IsFresh() && !entry.coin.IsSpent()) {
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }
            if (entry.fresh && it->second.coin.IsSpent()) {
                // Never written and now spent: drop it.
                EraseEntry(shard, ours);
                continue;
            }
            Detach(shard, *ours);
            SubUsage(shard, entry.coin.DynamicMemoryUsage());
            entry.coin = cursor.WillErase(*it) ? std::move(it->second.coin) : it->second.coin;
            // Not fresh: the entry may be in the database already (see CCoinsViewCache::BatchWrite).
        }
        entry.height = entry.coin.IsSpent() ? m_change_height : TagFor(entry.coin);
        Touch(entry);
        AddUsage(shard, entry.coin.DynamicMemoryUsage());
        entry.dirty = true;
        Attach(shard, *ours);
    }
    SetBestBlock(block_hash);
}

// ---------------------------------------------------------------------------------------------------------
// Sizes, eviction and the write-out

unsigned int CoinsStore::GetCacheSize() const
{
    return static_cast<unsigned int>(SumTotals(&Totals::entries));
}

size_t CoinsStore::GetDirtyCount() const noexcept
{
    return SumTotals(&Totals::dirty);
}

size_t CoinsStore::DynamicMemoryUsage() const
{
    // The maps as memusage::DynamicUsage() computes them, the shards' arrays of dirty lists, and the
    // coins: summed as they change (Totals), since the owner asks for every block it
    // connects and must neither queue behind a holder of a shard's lock nor read every shard.
    return SumTotals(&Totals::bytes);
}

size_t CoinsStore::DirtyBytesAtOrBelow(int height) const
{
    int64_t total{0};
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        for (size_t span{0}; span < shard.dirty_spans.size() && span <= SpanOf(height); ++span) {
            const Bucket& bucket{shard.dirty_spans[span]};
            if (span < SpanOf(height)) {
                total += bucket.bytes;
                continue;
            }
            for (const Node* node{bucket.head}; node; node = node->second.next) {
                if (node->second.height <= height) total += DirtyBytes(node->second);
            }
        }
    }
    return static_cast<size_t>(std::max<int64_t>(0, total));
}

std::optional<int> CoinsStore::OldestDirtyHeight() const
{
    std::optional<int> oldest;
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        for (const Bucket& bucket : shard.dirty_spans) {
            if (!bucket.head) continue;
            // The shard's first list with entries holds its lowest heights.
            for (const Node* node{bucket.head}; node; node = node->second.next) {
                if (!oldest || node->second.height < *oldest) oldest = node->second.height;
            }
            break;
        }
    }
    return oldest;
}

int CoinsStore::BoundForBytes(size_t bytes, int tip_height) const
{
    // Merge the shards' byte sums span by span and walk them from the oldest up; in the span the bound
    // lies in, the entries' own heights find it.
    std::vector<int64_t> merged;
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (merged.size() < shard.dirty_spans.size()) merged.resize(shard.dirty_spans.size());
        for (size_t span{0}; span < shard.dirty_spans.size(); ++span) merged[span] += shard.dirty_spans[span].bytes;
    }
    int64_t freed{0};
    for (size_t span{0}; span < merged.size(); ++span) {
        if (freed + merged[span] < static_cast<int64_t>(bytes)) {
            freed += merged[span];
            continue;
        }
        std::map<int32_t, int64_t> by_height;
        for (const Shard& shard : m_shards) {
            LOCK(shard.mutex);
            if (span >= shard.dirty_spans.size()) continue;
            for (const Node* node{shard.dirty_spans[span].head}; node; node = node->second.next) {
                by_height[node->second.height] += DirtyBytes(node->second);
            }
        }
        for (const auto& [h, b] : by_height) {
            freed += b;
            if (freed >= static_cast<int64_t>(bytes)) return std::min<int>(h, tip_height);
        }
        // The span lost entries between the two passes: the bound lies further up.
    }
    return tip_height;
}

size_t CoinsStore::EvictClean(size_t target_bytes)
{
    const SiteScope site{Site::WRITE_OUT};
    // Round the shards, a batch from the old end of each one's clean list at a time, so that the shards
    // age together and no shard's lock is held for long. An entry read since it was last passed over is
    // moved to the young end with its mark cleared instead; a second pass takes it.
    static constexpr size_t BATCH{256};
    size_t freed{0};
    bool progress{true};
    while (progress && DynamicMemoryUsage() > target_bytes) {
        progress = false;
        for (Shard& shard : m_shards) {
            if (DynamicMemoryUsage() <= target_bytes) break;
            LOCK(shard.mutex);
            for (size_t n{0}; n < BATCH && shard.clean_old; ++n) {
                Node& node{*shard.clean_old};
                progress = true;
                if (node.second.referenced) {
                    Detach(shard, node);
                    node.second.referenced = false;
                    Attach(shard, node);
                    continue;
                }
                freed += static_cast<size_t>(DirtyBytes(node.second));
                EraseEntry(shard, node);
            }
        }
    }
    return freed;
}

std::unique_ptr<CoinsStore::WriteOutRun> CoinsStore::BeginWriteOut(int bound_height, int head_height, const uint256& new_head, bool final)
{
    Assert(!m_run_in_flight);
    const SiteScope site{Site::WRITE_OUT};
    if (!final) {
        bool any{false};
        for (const Shard& shard : m_shards) {
            LOCK(shard.mutex);
            for (const Bucket& bucket : shard.dirty_spans) {
                if (!bucket.head) continue;
                // The shard's first list with entries holds its lowest heights.
                for (const Node* node{bucket.head}; node && !any; node = node->second.next) any = node->second.height <= bound_height;
                break;
            }
            if (any) break;
        }
        // Nothing to write and no head to move: the database already holds the bound.
        if (!any && bound_height <= m_written_height) return nullptr;
    }
    auto run{std::make_unique<WriteOutRun>()};
    run->new_head = new_head;
    run->old_head = m_written_head;
    run->bound_height = final ? head_height : bound_height;
    run->head_height = head_height;
    run->final = final;
    // Until every shard has been collected the bound stands in for the settled height (TagFor()).
    m_inflight_settled_height = run->bound_height;
    m_run_in_flight = true;
    return run;
}

//! About what a change adds to a database batch: the key and the framing, and for a coin its height, its
//! amount and its script. Within a few percent for the common scripts and seldom below, so that a chunk
//! collected up to the batch size is one batch.
static uint64_t BatchBytes(const CoinWrite& write)
{
    return write.coin ? 45 + write.coin->out.scriptPubKey.size() : 38;
}

//! Buffers of SortWrites(), kept between calls.
struct WriteSorter {
    std::vector<std::pair<uint64_t, uint32_t>> order;
    std::vector<CoinWrite> sorted;
};

/**
 * Put the changes from `from` on in the order of the database's keys, descending. The database takes a
 * batch one change at a time into a sorted structure, one writer at a time, and finds the place of a key
 * next to the last one it took far faster than the place of a key anywhere: the order costs a sort here,
 * on the thread that prepares the batch, and saves several times that inside the database's write. By
 * the leading bytes of the txid, which is the order of the keys but for the outputs of one transaction.
 */
static void SortWrites(std::vector<CoinWrite>& writes, size_t from, WriteSorter& sorter)
{
    const size_t count{writes.size() - from};
    if (count < 2) return;
    sorter.order.clear();
    sorter.order.reserve(count);
    for (size_t i{0}; i < count; ++i) {
        sorter.order.emplace_back(ReadBE64(writes[from + i].outpoint.hash.ToUint256().data()), static_cast<uint32_t>(i));
    }
    std::sort(sorter.order.begin(), sorter.order.end(), std::greater{});
    sorter.sorted.clear();
    sorter.sorted.reserve(count);
    for (const auto& [prefix, i] : sorter.order) sorter.sorted.push_back(std::move(writes[from + i]));
    std::move(sorter.sorted.begin(), sorter.sorted.end(), writes.begin() + from);
    sorter.sorted.clear();
}

void CoinsStore::CollectChunk(Shard& shard, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<Node*>& nodes) const
{
    AssertLockHeld(shard.mutex);
    for (Bucket& bucket : shard.dirty_spans) {
        // The oldest change this list keeps for a later run.
        int oldest_left{std::numeric_limits<int>::max()};
        for (Node* node{bucket.head}; node; node = node->second.next) {
            Entry& entry{node->second};
            if (!run.final && entry.height > run.bound_height) {
                oldest_left = std::min<int>(oldest_left, entry.height);
                continue;
            }
            if (size >= limit) return;
            writes.push_back({node->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(entry.coin)});
            nodes.push_back(node);
            size += BatchBytes(writes.back());
            ++run.count;
            run.bytes += static_cast<size_t>(DirtyBytes(entry));
            entry.collected = true;
            // Once the run lands the coin is in the database, so a spend meanwhile must produce an erase.
            entry.fresh = false;
        }
        if (oldest_left != std::numeric_limits<int>::max()) {
            // The oldest change left behind in this shard, the lists further up holding later spans: the
            // database holds the state one below it once the run has landed.
            run.oldest_left = std::min<int>(run.oldest_left, oldest_left);
            return;
        }
    }
}

void CoinsStore::SettleNode(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    if (!entry.collected) return; // Changed since: stays dirty with its new change.
    entry.collected = false;
    Assume(entry.dirty && !entry.fresh);
    Detach(shard, node);
    entry.dirty = false;
    if (entry.coin.IsSpent()) {
        // The erase is in the database; the entry has nothing left to say.
        EraseEntry(shard, node);
        return;
    }
    Attach(shard, node);
}

void CoinsStore::StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes)
{
    const SiteScope site{Site::WRITE_OUT};
    // A chunk is one database batch, the size the database's writes are measured to be good at: what the
    // run holds in memory beyond the tier is the chunk's coins and the batch made of them. At least one
    // change, whatever the limit.
    const uint64_t limit{std::max<uint64_t>(batch_bytes, 1)};
    uint64_t size{0};
    std::vector<CoinWrite> writes;
    std::vector<Node*> nodes;
    // The shards the chunk took entries from, each with the offset of its first node. A chunk fills across
    // shards, so that a small run is a few database batches and not one for every shard.
    std::vector<std::pair<Shard*, size_t>> spans;
    WriteSorter sorter;
    const auto write_chunk{[&] {
        if (writes.empty()) return;
        // Nothing below goes by a write's place: the nodes are settled by their own list.
        SortWrites(writes, 0, sorter);
        // Every chunk is written under the run's two heads; a crash after any of them is repaired from the pair.
        base->WriteRun(writes, run.new_head, run.old_head, /*final=*/false);
        for (size_t i{0}; i < spans.size(); ++i) {
            Shard& shard{*spans[i].first};
            const size_t end{i + 1 < spans.size() ? spans[i + 1].second : nodes.size()};
            LOCK(shard.mutex);
            for (size_t n{spans[i].second}; n < end; ++n) SettleNode(shard, *nodes[n]);
            ++shard.settles;
        }
        writes.clear();
        nodes.clear();
        spans.clear();
        size = 0;
    }};
    // From the highest shard down, as the flush goes (Flush()): the database's marker is in the run's
    // first batch, and the key it writes there sorts above every coin's.
    for (size_t index{m_shards.size()}; index-- > 0;) {
        Shard& shard{m_shards[index]};
        while (true) {
            const size_t before{nodes.size()};
            WITH_LOCK(shard.mutex, CollectChunk(shard, run, limit, size, writes, nodes));
            if (nodes.size() > before) spans.emplace_back(&shard, before);
            // Below the limit the shard has nothing more for this run, and the chunk goes on to the next.
            if (size < limit) break;
            write_chunk();
        }
    }
    write_chunk();
    run.settled_height = std::min(run.head_height, run.oldest_left == std::numeric_limits<int>::max() ? run.head_height : run.oldest_left - 1);
    Assume(run.final || run.settled_height >= run.bound_height);
    if (run.final) {
        base->WriteRun({}, run.new_head, run.old_head, /*final=*/true);
        return;
    }
    // The database now holds the settled head's state, with every outpoint the run wrote at its value in
    // new_head: the pair the next run starts from. On disk before the tier counts the run as written, so
    // that the old head recorded there is never below what pruning and the lag ceiling go by.
    run.settled_head = block_at(run.settled_height);
    base->WriteRun({}, run.new_head, run.settled_head, /*final=*/false);
}

std::unique_ptr<CoinsStore::WriteOutRun> CoinsStore::PrepareWriteOut(int bound_height, int head_height, const uint256& new_head, bool final)
{
    auto run{BeginWriteOut(bound_height, head_height, new_head, final)};
    if (!run) return nullptr;
    const SiteScope site{Site::WRITE_OUT};
    uint64_t size{0};
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        CollectChunk(shard, *run, std::numeric_limits<uint64_t>::max(), size, run->writes, run->nodes);
    }
    if (run->writes.empty() && !final) {
        m_run_in_flight = false;
        return nullptr;
    }
    run->settled_height = std::min(head_height, run->oldest_left == std::numeric_limits<int>::max() ? head_height : run->oldest_left - 1);
    Assume(final || run->settled_height >= bound_height);
    m_inflight_settled_height = run->settled_height;
    return run;
}

void CoinsStore::CommitWriteOut(const WriteOutRun& run)
{
    Assert(m_run_in_flight);
    const SiteScope site{Site::WRITE_OUT};
    for (Node* node : run.nodes) {
        Shard& shard{ShardFor(node->first)};
        LOCK(shard.mutex);
        SettleNode(shard, *node);
        ++shard.settles;
    }
    Assert(run.final || !run.settled_head.IsNull());
    m_written_head = run.final ? run.new_head : run.settled_head;
    m_written_height = run.settled_height;
    m_run_in_flight = false;
}

void CoinsStore::AbandonWriteOut()
{
    m_run_in_flight = false;
}

//! What the workers of a flush share.
struct CoinsStore::Wipe {
    const uint256 best;
    const uint256 old_head;
    //! The size of a batch.
    const uint64_t limit;
    //! A worker's write failed: the others take no further shard.
    std::atomic<bool> failed{false};
};

//! The most workers a flush has. A worker spends about a fifth of its time inside the database's write, and
//! the database takes one batch at a time, so a few more than five keep it busy and more would wait.
static constexpr size_t MAX_WIPE_WORKERS{8};

//! A worker of a flush: its shards, the batch it is filling, and the shards whose changes are in it.
struct CoinsStore::WipeWorker {
    //! The worker's shards, [begin, end), which it takes from the highest down.
    size_t begin{0};
    size_t end{0};
    //! Called once, when the worker's first batch is in the database.
    std::function<void()> after_first_batch;
    //! The batch: shard after shard from the highest down, each in the descending order of the keys.
    std::vector<CoinWrite> writes;
    WriteSorter sorter;
    //! What `writes` comes to as a database batch (BatchBytes()).
    uint64_t size{0};
    //! Shards the worker has emptied, locked until the batch with the last of their changes has landed.
    std::vector<Shard*> held;
    uint64_t changes{0};
    uint64_t batches{0};
    SteadyClock::duration write_time{0};
    SteadyClock::duration total_time{0};
    std::exception_ptr error;
};

void CoinsStore::WriteWipeBatch(const Wipe& wipe, WipeWorker& worker)
{
    if (!worker.writes.empty()) {
        const auto start{SteadyClock::now()};
        // Every batch is written under the same two heads; a crash after any of them is repaired from the pair.
        base->WriteRun(worker.writes, wipe.best, wipe.old_head, /*final=*/false);
        worker.write_time += SteadyClock::now() - start;
        worker.changes += worker.writes.size();
        ++worker.batches;
        worker.writes.clear();
        worker.size = 0;
        if (worker.after_first_batch) std::exchange(worker.after_first_batch, nullptr)();
    }
    for (Shard* shard : worker.held) shard->mutex.unlock();
    worker.held.clear();
}

uint64_t CoinsStore::WipeShard(Shard& shard, const Wipe& wipe, WipeWorker& worker)
{
    AssertLockHeld(shard.mutex);
    // The lists go with the entries.
    FreeDirtySpans(shard);
    MyTotals().dirty.fetch_sub(static_cast<int64_t>(shard.dirty_count), std::memory_order_relaxed);
    shard.dirty_count = 0;
    shard.clean_young = nullptr;
    shard.clean_old = nullptr;
    uint64_t added{0};
    // Where the shard's changes begin in the batch, or what is left of them after a batch went out.
    size_t from{worker.writes.size()};
    const auto add{[&](CoinWrite&& write) {
        const uint64_t bytes{BatchBytes(write)};
        added += bytes;
        worker.size += bytes;
        worker.writes.push_back(std::move(write));
    }};
    for (auto it{shard.map.begin()}; it != shard.map.end();) {
        Entry& entry{it->second};
        // As EraseEntry(), with the coin's usage taken before the coin is moved out.
        const size_t usage{entry.coin.DynamicMemoryUsage()};
        if (entry.dirty) {
            add({it->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(std::move(entry.coin))});
        }
        SubUsage(shard, usage);
        it = shard.map.erase(it);
        // A full batch goes out in the middle of a shard that is a large part of a batch by itself. The
        // shard stays locked: everything taken out of it so far is in this batch or an earlier one. A
        // batch of smaller shards ends between two of them (RunWipeWorker()), and when the last of them
        // turns out larger than expected the batch runs over the size by that much rather than be cut here.
        if (worker.size >= wipe.limit && added * 2 > wipe.limit) {
            SortWrites(worker.writes, from, worker.sorter);
            WriteWipeBatch(wipe, worker);
            from = 0;
        }
    }
    // The map hands its entries out in the order of their hashes.
    SortWrites(worker.writes, from, worker.sorter);
    shard.map.rehash(0);
    Resized(shard);
    ++shard.settles;
    return added;
}

void CoinsStore::RunWipeWorker(Wipe& wipe, WipeWorker& worker)
{
    const SiteScope site{Site::WRITE_OUT};
    const auto start{SteadyClock::now()};
    Shard* walked{nullptr};
    try {
        for (size_t index{worker.end}; index-- > worker.begin && !wipe.failed.load(std::memory_order_relaxed);) {
            Shard& shard{m_shards[index]};
            // The shard stays locked beyond its walk, among the others the batch took changes from, and
            // they are unlocked together and not in the reverse order: the lock order checks know of the
            // shard for its walk only.
            EnterCritical("shard.mutex", __FILE__, __LINE__, static_cast<ShardMutex::unique_lock::mutex_type*>(&shard.mutex));
            shard.mutex.lock();
            walked = &shard;
            const uint64_t added{WipeShard(shard, wipe, worker)};
            walked = nullptr;
            LeaveCritical();
            worker.held.push_back(&shard);
            // A batch ends between two shards when it is full, or when the next shard, taken to be as
            // large as this one, would not fit: the batch is then whole shards, a range of the database's
            // keys that no other batch reaches into. Shards of more than half a batch fill it to the
            // limit instead (WipeShard()), and two batches then share the range of the shard they were
            // cut in.
            if (worker.size >= wipe.limit || (added * 2 <= wipe.limit && worker.size + added > wipe.limit)) WriteWipeBatch(wipe, worker);
        }
        WriteWipeBatch(wipe, worker);
    } catch (const std::runtime_error&) {
        // The base's write failed. Flush() throws it on once every worker is through.
        worker.error = std::current_exception();
        wipe.failed.store(true, std::memory_order_relaxed);
        if (walked) {
            LeaveCritical();
            walked->mutex.unlock();
        }
        for (Shard* shard : worker.held) shard->mutex.unlock();
        worker.held.clear();
    }
    worker.total_time = SteadyClock::now() - start;
}

void CoinsStore::Flush(bool /*reallocate_cache*/)
{
    // The wipe, one pass: a shard's changes are written as its entries are taken out of it, the coins
    // moved rather than copied, so that a tier the size of the UTXO set is neither doubled in memory nor
    // walked three times (collect, settle, erase). A shard stays locked until the batch with the last
    // of its changes is in the database: a reader of the shard waits for the write rather than finding a
    // coin in neither place. Every call of WriteRun() names the same two heads, and a last one closes
    // the marker.
    //
    // The batches are made for the database to place, not to merge. A shard is a range of its keys
    // (ShardIndex()), and a batch is the changes of neighbouring shards, up to the batch size and ended
    // between two shards: a range of keys that no other batch of the flush reaches into. The database
    // turns each batch into one sorted file, and a file that overlaps none before it is put in place as
    // it is, where files that all cover the whole range of keys, as batches filled in hash order do, are
    // merged with each other and with everything below them again and again, the writers stopped while
    // the merging is behind. The shards are taken from the highest down, because the database's marker
    // goes out with the first batch and its head-blocks key sorts above every coin's: the file that
    // holds it then covers the top of the range, not all of it. The marker's other keys are in that
    // first batch and in the last, tiny one that closes it, and in none between (CCoinsViewDB::WriteRun()).
    // Inside a batch the changes are in the order of the keys as well (SortWrites()), a shard at a time.
    //
    // The database takes one batch at a time. With several workers the walk of the shards, the sort and
    // the serialization of a batch run while the database is busy with another worker's batch. Each
    // has a range of neighbouring shards of its own, so that a batch is still a range of keys no other
    // reaches into, and all but the one with the highest range start once its first batch, the
    // marker's, is in the database.
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    Assert(!m_run_in_flight);
    const SiteScope site{Site::WRITE_OUT};
    m_run_in_flight = true;
    const auto start{SteadyClock::now()};
    Wipe wipe{.best = best, .old_head = m_written_head, .limit = std::max<uint64_t>(m_batch_bytes, 1)};
    // A worker for every two batches there may be, so that each has batches to take turns with: a change
    // is 38 bytes of a batch at least.
    const uint64_t cores{std::max(1U, std::thread::hardware_concurrency())};
    const uint64_t wanted{m_base_concurrent ? GetDirtyCount() * uint64_t{38} / wipe.limit / 2 : 1};
    std::vector<WipeWorker> workers(std::clamp<uint64_t>(wanted, 1, std::min<uint64_t>({MAX_WIPE_WORKERS, cores, m_shards.size()})));
    for (size_t i{0}; i < workers.size(); ++i) {
        workers[i].begin = m_shards.size() * i / workers.size();
        workers[i].end = m_shards.size() * (i + 1) / workers.size();
    }
    {
        // The worker of the highest range runs here, and starts the others.
        std::vector<std::thread> helpers;
        WipeWorker& first{workers.back()};
        if (workers.size() > 1) {
            first.after_first_batch = [&] {
                for (size_t i{0}; i + 1 < workers.size(); ++i) {
                    helpers.emplace_back([&, i] {
                        util::ThreadRename("coinswipe");
                        RunWipeWorker(wipe, workers[i]);
                    });
                }
            };
        }
        RunWipeWorker(wipe, first);
        if (helpers.empty()) {
            // The highest range had nothing to write: the others one after the other, from the highest down.
            for (size_t i{workers.size() - 1}; i-- > 0;) RunWipeWorker(wipe, workers[i]);
        }
        for (std::thread& helper : helpers) helper.join();
    }
    for (const WipeWorker& worker : workers) {
        if (worker.error) std::rethrow_exception(worker.error);
    }
    // Every shard's changes are in the database: consistent with the best block again.
    base->WriteRun({}, best, wipe.old_head, /*final=*/true);
    m_written_head = best;
    m_written_height = head_height;
    m_run_in_flight = false;
    uint64_t changes{0};
    uint64_t batches{0};
    SteadyClock::duration total_time{0};
    SteadyClock::duration write_time{0};
    SteadyClock::duration longest{0};
    for (const WipeWorker& worker : workers) {
        changes += worker.changes;
        batches += worker.batches;
        total_time += worker.total_time;
        write_time += worker.write_time;
        longest = std::max(longest, worker.total_time);
    }
    LogDebug(BCLog::COINDB, "Flushed the coins tier in %.2fs: %u changes in %u batches of up to %.2f MiB; %u workers, %.2fs between them of which %.2fs in the database's write, the longest %.2fs",
             Ticks<SecondsDouble>(SteadyClock::now() - start), changes, batches, wipe.limit / double(1 << 20), workers.size(),
             Ticks<SecondsDouble>(total_time), Ticks<SecondsDouble>(write_time), Ticks<SecondsDouble>(longest));
}

void CoinsStore::Sync()
{
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    auto run{BeginWriteOut(/*bound_height=*/head_height, head_height, /*new_head=*/best, /*final=*/true)};
    Assert(run);
    StreamWriteOut(*run, /*block_at=*/{}, m_batch_bytes);
    CommitWriteOut(*run);
}

void CoinsStore::Reset() noexcept
{
    // The tier is never reset; a CCoinsViewCache child is. Nothing to do.
}

void CoinsStore::SanityCheck() const
{
    size_t total_bytes{0};
    size_t total_entries{0};
    size_t total_dirty{0};
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        size_t usage{0};
        size_t dirty{0};
        size_t clean_listed{0};
        std::map<size_t, std::pair<size_t, int64_t>> by_span;
        for (const auto& [outpoint, entry] : shard.map) {
            usage += entry.coin.DynamicMemoryUsage();
            if (entry.dirty) {
                ++dirty;
                auto& [count, bytes]{by_span[SpanOf(entry.height)]};
                ++count;
                bytes += DirtyBytes(entry);
            }
            // A spent entry is only ever kept to record a pending erase.
            assert(!entry.coin.IsSpent() || entry.dirty);
            // A fresh entry is dirty: it exists only because something changed it since the database last
            // knew it, and the database does not hold it.
            assert(!entry.fresh || entry.dirty);
            // Only a dirty entry awaits a run, and each entry is on the list its state calls for.
            assert(!entry.collected || entry.dirty);
            const List list{entry.dirty ? LIST_DIRTY : LIST_CLEAN};
            assert(entry.list == list);
            if (list == LIST_CLEAN) ++clean_listed;
        }
        assert(usage == shard.coins_usage);
        assert(dirty == shard.dirty_count);
        assert(shard.map.size() == shard.entries);
        assert(shard.map.bucket_count() == shard.buckets);
        total_bytes += memusage::DynamicUsage(shard.map) + memusage::DynamicUsage(shard.dirty_spans) + usage;
        total_entries += shard.map.size();
        total_dirty += dirty;
        // The lists hold exactly those entries, properly linked.
        assert(shard.dirty_spans.empty() == (dirty == 0));
        for (size_t span{0}; span < shard.dirty_spans.size(); ++span) {
            const Bucket& bucket{shard.dirty_spans[span]};
            size_t count{0};
            const Node* prev{nullptr};
            for (const Node* node{bucket.head}; node; prev = node, node = node->second.next) {
                assert(node->second.list == LIST_DIRTY && SpanOf(node->second.height) == span);
                assert(node->second.prev == prev);
                ++count;
            }
            const auto it{by_span.find(span)};
            if (it == by_span.end()) {
                assert(count == 0 && bucket.bytes == 0);
            } else {
                assert(it->second.first == count && it->second.second == bucket.bytes);
                by_span.erase(it);
            }
        }
        assert(by_span.empty());
        size_t count{0};
        const Node* prev{nullptr};
        for (const Node* node{shard.clean_young}; node; prev = node, node = node->second.next) {
            assert(node->second.list == LIST_CLEAN);
            assert(node->second.prev == prev);
            ++count;
        }
        assert(shard.clean_old == prev);
        assert(count == clean_listed);
    }
    // The totals are the shards' figures summed. (The check is the owner's, made with no change under way.)
    assert(DynamicMemoryUsage() == total_bytes);
    assert(GetCacheSize() == total_entries);
    assert(GetDirtyCount() == total_dirty);
}
