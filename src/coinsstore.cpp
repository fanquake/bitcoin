// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <logging.h>
#include <memusage.h>
#include <util/check.h>
#include <util/threadnames.h>
#include <util/threadpool.h>

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
    case Site::UNWIND: return "unwind";
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

bool CoinsStore::Pending(const Entry& entry) const
{
    const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
    return entry.coin.IsSpent() ? entry.block_spend && entry.height > confirmed
                                : entry.block_create && static_cast<int>(entry.coin.nHeight) > confirmed;
}

CoinsStore::Sight CoinsStore::See(const Entry& entry, int viewer_height) const
{
    if (entry.coin.IsSpent()) {
        // A plain spend, the viewer's own, or a lower block's: spent.
        if (!entry.block_spend || entry.height <= viewer_height) return Sight::NOTHING;
        return entry.height > m_confirmed_height.load(std::memory_order_relaxed) ? Sight::RECORD : Sight::NOTHING;
    }
    if (!entry.block_create) return Sight::COIN;
    const int created{static_cast<int>(entry.coin.nHeight)};
    if (created <= viewer_height) return Sight::COIN;
    return created > m_confirmed_height.load(std::memory_order_relaxed) ? Sight::NOTHING : Sight::COIN;
}

CoinsStore::BlockId CoinsStore::SlotAt(int height) const
{
    for (BlockId id{1}; id < m_window.size(); ++id) {
        const WindowBlock& slot{m_window[id]};
        // The status is published last by the writer; what it publishes (the height) is read after it.
        if (slot.status.load(std::memory_order_acquire) == BlockStatus::FREE) continue;
        if (slot.height.load(std::memory_order_relaxed) == height) return id;
    }
    return NO_BLOCK;
}

void CoinsStore::ChargeDelta(int height, int64_t delta) const
{
    const BlockId id{SlotAt(height)};
    if (id == NO_BLOCK) return;
    const WindowBlock& slot{m_window[id]};
    if (slot.status.load(std::memory_order_acquire) == BlockStatus::APPLIED) slot.dirty_delta.fetch_add(delta, std::memory_order_relaxed);
}

std::optional<Coin> CoinsStore::SeenCoin(Entry& entry, const COutPoint& outpoint, int viewer_height) const
{
    switch (See(entry, viewer_height)) {
    case Sight::COIN:
        Reference(entry);
        return entry.coin;
    case Sight::RECORD: return RecordCoin(entry.height, outpoint, viewer_height);
    case Sight::NOTHING: break;
    }
    return std::nullopt;
}

std::optional<Coin> CoinsStore::RecordCoin(int spender_height, const COutPoint& outpoint, int viewer_height) const
{
    const BlockId id{SlotAt(spender_height)};
    if (id == NO_BLOCK) return std::nullopt;
    const WindowBlock& slot{m_window[id]};
    LOCK(slot.mutex);
    if (slot.status.load(std::memory_order_relaxed) == BlockStatus::FREE || slot.height.load(std::memory_order_relaxed) != spender_height) return std::nullopt;
    const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
    for (auto it{slot.record.rbegin()}; it != slot.record.rend(); ++it) {
        if (!it->spend || it->dead || it->outpoint != outpoint) continue;
        // A coin an open block created, which is not there yet from where the viewer stands.
        const int created{static_cast<int>(it->coin.nHeight)};
        if (it->created && created > confirmed && created > viewer_height) return std::nullopt;
        return it->coin;
    }
    return std::nullopt;
}

std::optional<Coin> CoinsStore::RecordScan(const COutPoint& outpoint) const
{
    const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
    for (BlockId id{1}; id < m_window.size(); ++id) {
        const WindowBlock& slot{m_window[id]};
        if (slot.status.load(std::memory_order_acquire) == BlockStatus::FREE) continue;
        LOCK(slot.mutex);
        if (slot.status.load(std::memory_order_relaxed) == BlockStatus::FREE) continue;
        for (auto it{slot.record.rbegin()}; it != slot.record.rend(); ++it) {
            if (!it->spend || it->dead || it->outpoint != outpoint) continue;
            // A coin an open block created is no reader's.
            if (it->created && static_cast<int>(it->coin.nHeight) > confirmed) return std::nullopt;
            return it->coin;
        }
    }
    return std::nullopt;
}

void CoinsStore::Record(WindowBlock& mine, std::vector<Undo>& undo, int64_t& delta)
{
    if (delta != 0) mine.dirty_delta.fetch_add(std::exchange(delta, 0), std::memory_order_relaxed);
    if (undo.empty()) return;
    LOCK(mine.mutex);
    mine.record.insert(mine.record.end(), std::make_move_iterator(undo.begin()), std::make_move_iterator(undo.end()));
    undo.clear();
}

// ---------------------------------------------------------------------------------------------------------
// Lookups

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    return LookupFor(NO_BLOCK, outpoint);
}

std::optional<Coin> CoinsStore::LookupFor(BlockId viewer, const COutPoint& outpoint) const
{
    const SiteScope site{Site::LOOKUP};
    const int viewer_height{ViewerHeight(viewer)};
    Shard& shard{ShardFor(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) return SeenCoin(it->second, outpoint, viewer_height);
            settles = shard.settles;
        }
        // A miss: read the base with the shard unlocked, then re-take the shard and insert unless another
        // thread brought the entry in meanwhile (in which case its state wins, whatever it is).
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        // A write-out settled entries of the shard meanwhile: the coin may have been spent, written as an
        // erase and dropped from the tier since the miss, with the base read before the erase. Look again.
        if (shard.settles != settles) continue;
        auto it{shard.map.find(outpoint)};
        if (it != shard.map.end()) return SeenCoin(it->second, outpoint, viewer_height);
        if (!coin) {
            // In neither place. A confirmed coin the database never held and an open block spent is in that
            // block's record alone, where a reader finds it. An applier does not look there: a lower block
            // spending what a higher one spent is a chain that fails, and is connected again in order.
            if (viewer == NO_BLOCK && m_open_slots.load(std::memory_order_relaxed) > 0) return RecordScan(outpoint);
            return std::nullopt;
        }
        Assert(!coin->IsSpent());
        it = shard.map.try_emplace(outpoint).first;
        Resized(shard);
        it->second.coin = *coin;
        it->second.height = coin->nHeight;
        AddUsage(shard, it->second.coin.DynamicMemoryUsage());
        Attach(shard, *it);
        return coin;
    }
}

std::optional<Coin> CoinsStore::PeekCoinFor(BlockId viewer, const COutPoint& outpoint) const
{
    const SiteScope site{Site::LOOKUP};
    const int viewer_height{ViewerHeight(viewer)};
    Shard& shard{ShardFor(outpoint)};
    {
        LOCK(shard.mutex);
        if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) return SeenCoin(it->second, outpoint, viewer_height);
    }
    std::optional<Coin> coin{base->PeekCoin(outpoint)};
    if (coin || viewer != NO_BLOCK || m_open_slots.load(std::memory_order_relaxed) == 0) return coin;
    // As LookupFor(): a reader's coin may be in an open block's record alone.
    LOCK(shard.mutex);
    if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) return SeenCoin(it->second, outpoint, viewer_height);
    return RecordScan(outpoint);
}

std::optional<Coin> CoinsStore::GetCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint);
}

std::optional<Coin> CoinsStore::PeekCoin(const COutPoint& outpoint) const
{
    return PeekCoinFor(NO_BLOCK, outpoint);
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
    if (it == shard.map.end()) return false;
    switch (See(it->second, /*viewer_height=*/-1)) {
    case Sight::COIN: return true;
    case Sight::RECORD: return RecordCoin(it->second.height, outpoint, /*viewer_height=*/-1).has_value();
    case Sight::NOTHING: break;
    }
    return false;
}

static const Coin STORE_EMPTY_COIN;

const Coin& CoinsStore::AccessCoin(const COutPoint& outpoint) const
{
    // Bring the entry in if needed, then hand out a reference to it. Valid while the owner's lock keeps the
    // entry from being erased or replaced.
    std::optional<Coin> coin{Lookup(outpoint)};
    if (!coin) return STORE_EMPTY_COIN;
    Shard& shard{ShardFor(outpoint)};
    {
        LOCK(shard.mutex);
        const auto it{shard.map.find(outpoint)};
        if (it != shard.map.end() && See(it->second, /*viewer_height=*/-1) == Sight::COIN) {
            Reference(it->second);
            return it->second.coin;
        }
    }
    // The coin is in the record of the open block that spent it: a copy, good until the thread's next call.
    static thread_local Coin t_coin;
    t_coin = std::move(*coin);
    return t_coin;
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
    SetTierHeight(height);
}

// ---------------------------------------------------------------------------------------------------------
// Confirmed changes without a block identity

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
    entry.block_create = false;
    entry.block_spend = false;
    entry.owed = false;
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
    entry.owed = false;
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
    entry.block_create = false;
    entry.block_spend = false;
    entry.owed = false;
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
        entry.block_create = false;
        entry.block_spend = false;
        entry.owed = false;
        Touch(entry);
        AddUsage(shard, entry.coin.DynamicMemoryUsage());
        entry.dirty = true;
        Attach(shard, *ours);
    }
    SetBestBlock(block_hash);
}

// ---------------------------------------------------------------------------------------------------------
// Changes with a block identity

CoinsStore::BlockId CoinsStore::BeginBlock(int height)
{
    const BlockId id{ClaimSlot(height)};
    // More blocks open than the widest window can hold: a caller's error, never the chain's.
    assert(id != NO_BLOCK);
    return id;
}

CoinsStore::BlockId CoinsStore::ClaimSlot(int height)
{
    LOCK(m_window_mutex);
    for (BlockId id{1}; id < m_window.size(); ++id) {
        WindowBlock& slot{m_window[id]};
        if (slot.status.load(std::memory_order_relaxed) != BlockStatus::FREE) continue;
        // Counted before the slot shows: a reader that finds the count at zero has no record to look into.
        m_open_slots.fetch_add(1, std::memory_order_relaxed);
        LOCK(slot.mutex);
        slot.conflicts = 0;
        slot.record.clear();
        slot.dirty_delta.store(0, std::memory_order_relaxed);
        slot.height.store(height, std::memory_order_relaxed);
        slot.status.store(BlockStatus::APPLIED, std::memory_order_release);
        return id;
    }
    return NO_BLOCK;
}

int CoinsStore::BlockHeight(BlockId block) const
{
    Assert(block < m_window.size());
    return m_window[block].height.load(std::memory_order_relaxed);
}

uint32_t CoinsStore::ConflictCount(BlockId block) const
{
    Assert(block < m_window.size());
    LOCK(m_window[block].mutex);
    return m_window[block].conflicts;
}

size_t CoinsStore::OpenBlocks() const
{
    size_t open{0};
    for (const WindowBlock& b : m_window) {
        if (b.status.load(std::memory_order_relaxed) == BlockStatus::APPLIED) ++open;
    }
    return open;
}

bool CoinsStore::CreateLocked(Shard& shard, int height, const COutPoint& outpoint, Coin&& coin, bool possible_overwrite, std::vector<Undo>& undo, int64_t& delta)
{
    AssertLockHeld(shard.mutex);
    // The coin's height is what tells the create from a confirmed one (Pending()).
    Assume(static_cast<int>(coin.nHeight) == height);
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    Entry& entry{it->second};
    // Whether the entry takes the block's height as its change height.
    bool retag{true};
    if (inserted) {
        Resized(shard);
        entry.fresh = !possible_overwrite;
        ++delta;
    } else {
        if (!possible_overwrite && See(entry, height) != Sight::NOTHING) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        if (Pending(entry)) {
            // An open block's spend under a create is an outpoint made twice (BIP30), and so is a lower
            // open block's create: not this block's to settle here. It fails, and is connected again alone.
            if (entry.coin.IsSpent()) return false;
            const int other{static_cast<int>(entry.coin.nHeight)};
            if (other < height) return false;
            if (other > height) {
                // A higher block created this outpoint first: a duplicate it could not see, since this
                // block had not produced the original yet. The higher block is at fault and fails at its
                // confirmation; its entry is this block's now, with what it added to the dirty entries.
                const int64_t moved{entry.owed ? 0 : 1};
                if (const BlockId id{SlotAt(other)}; id != NO_BLOCK) {
                    WindowBlock& charged{m_window[id]};
                    LOCK(charged.mutex);
                    if (charged.status.load(std::memory_order_relaxed) == BlockStatus::APPLIED) ++charged.conflicts;
                    charged.dirty_delta.fetch_sub(moved, std::memory_order_relaxed);
                }
                delta += moved;
                entry.fresh = entry.fresh && !possible_overwrite;
            }
            // Under an erase still owed the entry keeps that change's height, as below.
            retag = !entry.owed;
        } else {
            // A confirmed entry. Dirty, it records a spentness the database does not have yet: the erase is
            // owed. The create is written over it once the block is confirmed; until then, and if the
            // block is unwound, the erase is owed still, so the entry stays at the height of the change it
            // owes (see CollectChunk() and UnwindItem()). What is owed is an erase: a dirty unspent coin
            // here (possible_overwrite over a coin the database does not have yet, as at the two BIP30
            // duplicates) would owe its previous value instead, which is not kept.
            entry.fresh = false;
            entry.owed = entry.dirty;
            Assume(!entry.owed || entry.coin.IsSpent());
            retag = !entry.owed;
            if (!entry.owed) ++delta;
        }
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
    }
    entry.coin = std::move(coin);
    if (retag) entry.height = height;
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    entry.block_create = true;
    entry.block_spend = false;
    // Dirty from now on, where the write-out will find it once the block is confirmed; it passes over the
    // entry until then (Pending()).
    entry.dirty = true;
    Touch(entry);
    Attach(shard, *it);
    undo.push_back(Undo{.coin = {}, .outpoint = outpoint});
    return true;
}

void CoinsStore::ApplyCreate(BlockId block, const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    assert(block != NO_BLOCK);
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    const SiteScope site{Site::APPLY};
    const int height{BlockHeight(block)};
    Shard& shard{ShardFor(outpoint)};
    WindowBlock& mine{m_window[block]};
    std::vector<Undo> undo;
    int64_t delta{0};
    bool made;
    {
        LOCK(shard.mutex);
        made = CreateLocked(shard, height, outpoint, std::move(coin), possible_overwrite, undo, delta);
        Record(mine, undo, delta);
    }
    if (!made) {
        LOCK(mine.mutex);
        ++mine.conflicts;
    }
}

CoinsStore::SpendResult CoinsStore::SpendLocked(Shard& shard, int height, const COutPoint& outpoint, Coin* copy, const Coin* from_base, std::vector<Undo>& undo, int64_t& delta)
{
    AssertLockHeld(shard.mutex);
    auto it{shard.map.find(outpoint)};
    if (it == shard.map.end()) {
        // Not in the tier (the prefetch reads without inserting): the caller reads the base with the shard
        // unlocked. The database's coin goes to the record, and the tier keeps the spend, for the erase.
        if (!from_base) return SpendResult::MISS;
        it = shard.map.try_emplace(outpoint).first;
        Resized(shard);
        Entry& marker{it->second};
        marker.height = height;
        marker.dirty = true;
        marker.block_spend = true;
        Attach(shard, *it);
        ++delta;
        if (copy) *copy = *from_base;
        undo.push_back(Undo{.coin = *from_base, .outpoint = outpoint, .height = static_cast<int32_t>(from_base->nHeight), .spend = true});
        return SpendResult::DONE;
    }
    Entry& entry{it->second};
    switch (See(entry, height)) {
    case Sight::NOTHING: return SpendResult::CONFLICT;
    case Sight::RECORD: {
        // A higher block spent the coin first. This block displaces it: the spend, with the coin in the
        // record, is this block's now, and the displaced block fails at its confirmation.
        const int other{entry.height};
        const BlockId id{SlotAt(other)};
        if (id == NO_BLOCK) return SpendResult::CONFLICT;
        WindowBlock& theirs{m_window[id]};
        {
            LOCK(theirs.mutex);
            const BlockStatus status{theirs.status.load(std::memory_order_relaxed)};
            if (status == BlockStatus::FREE || theirs.height.load(std::memory_order_relaxed) != other) return SpendResult::CONFLICT;
            const auto item{std::find_if(theirs.record.rbegin(), theirs.record.rend(), [&](const Undo& u) { return u.spend && !u.dead && u.outpoint == outpoint; })};
            if (item == theirs.record.rend()) return SpendResult::CONFLICT;
            // A coin a block above this one created is not there yet.
            const int created{static_cast<int>(item->coin.nHeight)};
            if (item->created && created > height && created > m_confirmed_height.load(std::memory_order_relaxed)) return SpendResult::CONFLICT;
            if (status == BlockStatus::APPLIED) ++theirs.conflicts;
            const int64_t moved{item->dirty ? 0 : 1};
            theirs.dirty_delta.fetch_sub(moved, std::memory_order_relaxed);
            delta += moved;
            if (copy) *copy = item->coin;
            undo.push_back(std::move(*item));
            item->dead = true;
        }
        Detach(shard, *it);
        entry.height = height;
        Touch(entry);
        Attach(shard, *it);
        return SpendResult::DONE;
    }
    case Sight::COIN: break;
    }
    Undo item{.coin = {}, .outpoint = outpoint, .height = entry.height, .spend = true, .dirty = entry.dirty, .fresh = entry.fresh, .created = entry.block_create, .owed = entry.owed};
    if (copy) *copy = entry.coin;
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    item.coin = std::move(entry.coin);
    if (entry.fresh) {
        // Never written: the database has nothing to erase and the tier nothing to keep. The coin is in
        // the record, for the unwind and for whoever is still to see it.
        EraseDetached(shard, it);
        --delta;
    } else {
        entry.coin.Clear();
        if (!entry.dirty) ++delta;
        entry.height = height;
        entry.dirty = true;
        entry.block_spend = true;
        entry.block_create = false;
        entry.owed = false;
        Touch(entry);
        Attach(shard, *it);
    }
    undo.push_back(std::move(item));
    return SpendResult::DONE;
}

bool CoinsStore::ApplySpend(BlockId block, const COutPoint& outpoint, Coin* copy)
{
    assert(block != NO_BLOCK);
    const SiteScope site{Site::APPLY};
    const int height{BlockHeight(block)};
    Shard& shard{ShardFor(outpoint)};
    WindowBlock& mine{m_window[block]};
    std::vector<Undo> undo;
    int64_t delta{0};
    std::optional<Coin> from_base;
    uint64_t settles{0};
    while (true) {
        SpendResult result;
        {
            LOCK(shard.mutex);
            // The base was read with the shard unlocked. If a write-out settled entries of the shard
            // meanwhile, a lower block may have spent the coin and had the erase written and the entry
            // dropped since the miss, with the base read before the erase: the coin read is then stale,
            // and inserting it would hide the double spend. Read again.
            if (from_base && shard.settles != settles) from_base.reset();
            result = SpendLocked(shard, height, outpoint, copy, from_base ? &*from_base : nullptr, undo, delta);
            settles = shard.settles;
            Record(mine, undo, delta);
        }
        if (result == SpendResult::DONE) return true;
        if (result == SpendResult::CONFLICT) break;
        // Not in the tier: the base, with the shard unlocked. The block saw the coin when it checked its
        // inputs; a coin the base does not have either is a conflict.
        from_base = base->GetCoin(outpoint);
        if (!from_base) break;
        Assert(!from_base->IsSpent());
    }
    LOCK(mine.mutex);
    ++mine.conflicts;
    return false;
}

template <typename Work>
void CoinsStore::VisitShards(BlockId block, const std::vector<uint32_t>& shard_of, Work work) const
{
    const size_t shards{m_shards.size()};
    // A counting sort of the items by shard: shard s has the items order[starts[s]] to order[starts[s + 1]].
    std::vector<uint32_t> starts(shards + 1, 0);
    for (const uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < shards; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(shard_of.size());
    {
        std::vector<uint32_t> next(starts.begin(), starts.end() - 1);
        for (uint32_t i{0}; i < shard_of.size(); ++i) order[next[shard_of[i]]++] = i;
    }
    const auto items{[&](size_t s) { return std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]); }};
    std::vector<uint32_t> busy;
    size_t s{block * shards / MAX_WINDOW_SLOTS % shards};
    for (size_t n{0}; n < shards; ++n, s = s + 1 == shards ? 0 : s + 1) {
        if (starts[s] == starts[s + 1]) continue;
        Shard& shard{m_shards[s]};
        TRY_LOCK(shard.mutex, lock);
        if (!lock) {
            busy.push_back(s);
            continue;
        }
        work(shard, items(s));
    }
    for (const uint32_t b : busy) {
        Shard& shard{m_shards[b]};
        LOCK(shard.mutex);
        work(shard, items(b));
    }
}

void CoinsStore::ApplyBatch(BlockId block, std::vector<Change>&& changes)
{
    assert(block != NO_BLOCK);
    if (changes.empty()) return;
    const SiteScope site{Site::APPLY};
    const int height{BlockHeight(block)};
    // Grouped by shard, so that each shard is taken once.
    std::vector<Change*> applied;
    std::vector<uint32_t> shard_of;
    applied.reserve(changes.size());
    shard_of.reserve(changes.size());
    for (Change& change : changes) {
        if (!change.coin.IsSpent() && change.coin.out.scriptPubKey.IsUnspendable()) continue; // as ApplyCreate()
        applied.push_back(&change);
        shard_of.push_back(ShardIndex(change.outpoint));
    }
    WindowBlock& mine{m_window[block]};
    {
        LOCK(mine.mutex);
        mine.record.reserve(mine.record.size() + applied.size());
    }
    // A shard's changes, until they are in the record.
    std::vector<Undo> undo;
    int64_t delta{0};
    uint32_t conflicts{0};
    std::vector<const Change*> misses;
    VisitShards(block, shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex, !m_window_mutex) {
        for (const uint32_t i : items) {
            Change* change{applied[i]};
            if (change->coin.IsSpent()) {
                switch (SpendLocked(shard, height, change->outpoint, nullptr, nullptr, undo, delta)) {
                case SpendResult::DONE: break;
                case SpendResult::CONFLICT: ++conflicts; break;
                case SpendResult::MISS: misses.push_back(change); break;
                }
            } else if (!CreateLocked(shard, height, change->outpoint, std::move(change->coin), change->possible_overwrite, undo, delta)) {
                ++conflicts;
            }
        }
        Record(mine, undo, delta);
    });
    if (conflicts != 0) {
        LOCK(mine.mutex);
        mine.conflicts += conflicts;
    }
    // Spends of coins the tier no longer holds (evicted since the block read them): one by one, via the base.
    for (const Change* change : misses) ApplySpend(block, change->outpoint, nullptr);
}

std::vector<std::pair<COutPoint, Coin>> CoinsStore::FetchFor(BlockId viewer, std::vector<COutPoint>&& outpoints) const
{
    const SiteScope site{Site::FETCH};
    const int viewer_height{ViewerHeight(viewer)};
    std::vector<uint32_t> shard_of;
    shard_of.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) shard_of.push_back(ShardIndex(outpoint));
    std::vector<std::pair<COutPoint, Coin>> found;
    found.reserve(outpoints.size());
    VisitShards(viewer, shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex, !m_window_mutex) {
        for (const uint32_t i : items) {
            const COutPoint& outpoint{outpoints[i]};
            const auto it{shard.map.find(outpoint)};
            if (it == shard.map.end() || See(it->second, viewer_height) != Sight::COIN) continue;
            Reference(it->second);
            found.emplace_back(outpoint, it->second.coin);
        }
    });
    return found;
}

void CoinsStore::ConfirmBlock(BlockId block, ThreadPool* pool)
{
    assert(block != NO_BLOCK);
    Assert(block < m_window.size());
    WindowBlock& slot{m_window[block]};
    std::vector<Undo> record;
    {
        LOCK(slot.mutex);
        Assert(slot.status.load(std::memory_order_relaxed) == BlockStatus::APPLIED);
        // All there is to it: from here on the block's creates and spent markers read as anyone's
        // (Pending(), See()). They are dirty already, on the lists of the block's height.
        m_confirmed_height.store(slot.height.load(std::memory_order_relaxed), std::memory_order_release);
        record.swap(slot.record);
        slot.conflicts = 0;
        slot.dirty_delta.store(0, std::memory_order_relaxed);
        slot.status.store(BlockStatus::FREE, std::memory_order_release);
    }
    m_open_slots.fetch_sub(1, std::memory_order_relaxed);
    if (!pool || record.empty()) return;
    // The record's coins are freed where the task ends: on the pool, or here if the pool takes no more.
    auto moved{std::make_shared<std::vector<Undo>>(std::move(record))};
    if (pool->Submit([moved] { moved->clear(); })) return;
}

void CoinsStore::UnwindItem(Shard& shard, int height, Undo&& item)
{
    AssertLockHeld(shard.mutex);
    auto it{shard.map.find(item.outpoint)};
    if (!item.spend) {
        // The block's create, unless the outpoint is another block's by now: a lower block's that took
        // it over, or the marker of a higher block's spend (which goes with that block's unwind).
        if (it == shard.map.end()) return;
        Entry& entry{it->second};
        if (entry.coin.IsSpent() || !entry.block_create || static_cast<int>(entry.coin.nHeight) != height) return;
        if (!entry.owed) {
            EraseEntry(shard, it);
            return;
        }
        // The erase the create went over is owed still: a spent entry again, at the height it kept.
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
        entry.coin.Clear();
        entry.fresh = false;
        entry.block_create = false;
        entry.owed = false;
        Touch(entry);
        Attach(shard, *it);
        return;
    }
    // A coin an open block created goes with that block: if it is no longer applied (unwound before this
    // one, or this block itself), there is no coin to put back.
    bool gone{false};
    if (const int created{static_cast<int>(item.coin.nHeight)}; item.created && created > m_confirmed_height.load(std::memory_order_relaxed)) {
        const BlockId creator{SlotAt(created)};
        gone = creator == NO_BLOCK || m_window[creator].status.load(std::memory_order_acquire) != BlockStatus::APPLIED;
    }
    const auto restore{[&](Entry& entry) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        entry.coin = std::move(item.coin);
        AddUsage(shard, entry.coin.DynamicMemoryUsage());
        entry.height = item.height;
        entry.dirty = item.dirty;
        entry.fresh = item.fresh;
        // Clean, the coin has been written since (CollectRecords()) and is anyone's.
        entry.block_create = item.created && item.dirty;
        entry.owed = item.owed;
        entry.block_spend = false;
    }};
    if (it == shard.map.end()) {
        // The spend erased the entry: a coin the database never held.
        if (gone) return;
        it = shard.map.try_emplace(item.outpoint).first;
        Resized(shard);
        restore(it->second);
        Attach(shard, *it);
        return;
    }
    Entry& entry{it->second};
    // The marker the spend left; anything else is another block's.
    if (!entry.coin.IsSpent() || !entry.block_spend || entry.height != height) return;
    if (gone && !item.owed) {
        EraseEntry(shard, it);
        return;
    }
    Detach(shard, *it);
    if (gone) {
        // The erase the create went over is owed still.
        entry.height = item.height;
        entry.block_spend = false;
    } else {
        restore(entry);
    }
    Touch(entry);
    Attach(shard, *it);
}

void CoinsStore::UnwindBlock(BlockId block)
{
    assert(block != NO_BLOCK);
    Assert(block < m_window.size());
    WindowBlock& slot{m_window[block]};
    const SiteScope site{Site::UNWIND};
    const int height{slot.height.load(std::memory_order_relaxed)};
    size_t left;
    {
        LOCK(slot.mutex);
        Assert(slot.status.load(std::memory_order_relaxed) == BlockStatus::APPLIED);
        slot.status.store(BlockStatus::UNWOUND, std::memory_order_release);
        left = slot.record.size();
    }
    // The last change first. A shard is taken before the slot, as everywhere: the outpoint is read for
    // its shard, and the item taken out of the record once the shard is held, unless a lower block took
    // the spend over meanwhile.
    while (left-- > 0) {
        const COutPoint outpoint{WITH_LOCK(slot.mutex, return slot.record[left].outpoint)};
        Shard& shard{ShardFor(outpoint)};
        LOCK(shard.mutex);
        Undo item;
        {
            LOCK(slot.mutex);
            Undo& recorded{slot.record[left]};
            if (recorded.dead) continue;
            recorded.dead = true;
            item = std::move(recorded);
        }
        UnwindItem(shard, height, std::move(item));
    }
    {
        LOCK(slot.mutex);
        std::vector<Undo>{}.swap(slot.record);
        slot.conflicts = 0;
        slot.dirty_delta.store(0, std::memory_order_relaxed);
        slot.status.store(BlockStatus::FREE, std::memory_order_release);
    }
    m_open_slots.fetch_sub(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------
// Sizes, eviction and the write-out

unsigned int CoinsStore::GetCacheSize() const
{
    return static_cast<unsigned int>(SumTotals(&Totals::entries));
}

size_t CoinsStore::GetDirtyCount() const noexcept
{
    // Every dirty entry, less what the open blocks added: their changes are dirty where the write-out
    // will find them, and count once the block is confirmed.
    int64_t dirty{static_cast<int64_t>(SumTotals(&Totals::dirty))};
    if (m_open_slots.load(std::memory_order_relaxed) > 0) {
        for (const WindowBlock& slot : m_window) {
            if (slot.status.load(std::memory_order_acquire) == BlockStatus::APPLIED) dirty -= slot.dirty_delta.load(std::memory_order_relaxed);
        }
    }
    return static_cast<size_t>(std::max<int64_t>(0, dirty));
}

size_t CoinsStore::DynamicMemoryUsage() const
{
    // The maps as memusage::DynamicUsage() computes them, the shards' arrays of dirty lists, and the
    // coins: summed as they change (Totals), since the connector asks for every block
    // it publishes and must neither queue behind an applier holding a shard nor read every shard.
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
        std::optional<int> lowest;
        for (size_t span{0}; span < shard.dirty_spans.size(); ++span) {
            // The lists further up hold later heights.
            if (lowest && SpanOf(*lowest) < span) break;
            for (const Node* node{shard.dirty_spans[span].head}; node; node = node->second.next) {
                const Entry& entry{node->second};
                int height{entry.height};
                if (Pending(entry)) {
                    // An open block's change is no confirmed one; the erase its create went over is.
                    if (!entry.owed) continue;
                } else if (entry.owed) {
                    // A confirmed create over an erase still owed: the coin is the change now.
                    height = static_cast<int>(entry.coin.nHeight);
                }
                if (!lowest || height < *lowest) lowest = height;
            }
        }
        if (lowest && (!oldest || *lowest < *oldest)) oldest = lowest;
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
                for (const Node* node{bucket.head}; node && !any; node = node->second.next) {
                    // An open block's own change is not the run's, the erase its create went over is.
                    any = node->second.height <= bound_height && (node->second.owed || !Pending(node->second));
                }
                break;
            }
            if (any) break;
        }
        if (!any && m_open_slots.load(std::memory_order_relaxed) > 0) {
            // A confirmed change an open block spent is in the block's record alone (CollectRecords()).
            const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
            for (const WindowBlock& slot : m_window) {
                if (slot.status.load(std::memory_order_acquire) != BlockStatus::APPLIED) continue;
                LOCK(slot.mutex);
                for (const Undo& item : slot.record) {
                    if (!item.spend || item.dead || item.height > bound_height) continue;
                    const bool pending_create{item.created && static_cast<int>(item.coin.nHeight) > confirmed};
                    if (item.owed || (item.dirty && !pending_create)) {
                        any = true;
                        break;
                    }
                }
                if (any) break;
            }
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
        for (Node* node{bucket.head}, *next{nullptr}; node; node = next) {
            next = node->second.next;
            Entry& entry{node->second};
            // An open block's change is on these lists from the moment it is made, and is passed over
            // until the block is confirmed: its height is above every bound. What the block changed is
            // not: a confirmed coin it spent is in its record (CollectRecords()), and an erase its create
            // went over is owed by the entry, which kept that change's height.
            const bool pending{Pending(entry)};
            if (pending && !entry.owed) continue;
            if (!run.final && entry.height > run.bound_height) {
                oldest_left = std::min<int>(oldest_left, entry.height);
                continue;
            }
            if (size >= limit) return;
            if (const int created{static_cast<int>(entry.coin.nHeight)}; entry.owed && (pending || (!run.final && created > run.bound_height))) {
                // The create is not the run's to write, the erase under it is: left behind, it would be
                // missing from a database whose marker closes at the run's head. Written, the entry owes
                // nothing but its coin, at the coin's height.
                writes.push_back({node->first, std::nullopt});
                size += BatchBytes(writes.back());
                ++run.count;
                Detach(shard, *node);
                entry.owed = false;
                entry.height = created;
                Attach(shard, *node);
                if (pending) {
                    // One confirmed change fewer; the entry is the open block's alone now.
                    ChargeDelta(created, 1);
                } else {
                    // A confirmed coin above the bound, for a later run.
                    run.oldest_left = std::min<int>(run.oldest_left, created);
                }
                continue;
            }
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
    // What the database holds is anyone's.
    entry.block_create = false;
    entry.block_spend = false;
    entry.owed = false;
    if (entry.coin.IsSpent()) {
        // The erase is in the database; the entry has nothing left to say.
        EraseEntry(shard, node);
        return;
    }
    Attach(shard, node);
}

int CoinsStore::CollectRecords(int bound_height, bool final, std::vector<CoinWrite>& writes, size_t& count)
{
    int oldest_left{std::numeric_limits<int>::max()};
    if (m_open_slots.load(std::memory_order_relaxed) == 0) return oldest_left;
    // A confirmed coin an open block spent is in that block's record and nowhere else: the tier has the
    // block's marker, or nothing. If the database does not have the coin yet, the run writes it from
    // there. The items are found with the slot held alone, and each is taken with its shard held first.
    struct Candidate {
        BlockId slot;
        size_t index;
        COutPoint outpoint;
    };
    std::vector<Candidate> candidates;
    for (BlockId id{1}; id < m_window.size(); ++id) {
        WindowBlock& slot{m_window[id]};
        if (slot.status.load(std::memory_order_acquire) != BlockStatus::APPLIED) continue;
        LOCK(slot.mutex);
        if (slot.status.load(std::memory_order_relaxed) != BlockStatus::APPLIED) continue;
        const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
        for (size_t i{0}; i < slot.record.size(); ++i) {
            const Undo& item{slot.record[i]};
            if (!item.spend || item.dead) continue;
            // Clean, the database has the coin. An open block's coin is no confirmed change, but the erase
            // its create went over is.
            const bool pending_create{item.created && static_cast<int>(item.coin.nHeight) > confirmed};
            if (!item.owed && (!item.dirty || pending_create)) continue;
            if (!final && item.height > bound_height) {
                oldest_left = std::min<int>(oldest_left, item.height);
                continue;
            }
            candidates.push_back({id, i, item.outpoint});
        }
    }
    for (const Candidate& candidate : candidates) {
        const COutPoint& outpoint{candidate.outpoint};
        Shard& shard{ShardFor(outpoint)};
        LOCK(shard.mutex);
        auto it{shard.map.find(outpoint)};
        BlockId id{candidate.slot};
        size_t hint{candidate.index};
        const bool marker{it != shard.map.end() && it->second.coin.IsSpent() && it->second.block_spend && Pending(it->second)};
        if (marker) {
            // The spend is the block's whose marker this is: a lower block may have taken it over since.
            if (const BlockId now{SlotAt(it->second.height)}; now != id) {
                id = now;
                hint = std::numeric_limits<size_t>::max();
            }
        } else if (it != shard.map.end()) {
            continue; // decided since: the entry is anyone's
        }
        if (id == NO_BLOCK) continue;
        WindowBlock& slot{m_window[id]};
        LOCK(slot.mutex);
        if (slot.status.load(std::memory_order_relaxed) != BlockStatus::APPLIED) continue;
        const auto live{[&](const Undo& u) { return u.spend && !u.dead && u.outpoint == outpoint; }};
        Undo* item{nullptr};
        if (hint < slot.record.size() && live(slot.record[hint])) {
            item = &slot.record[hint];
        } else if (marker) {
            if (const auto found{std::find_if(slot.record.rbegin(), slot.record.rend(), live)}; found != slot.record.rend()) item = &*found;
        }
        if (!item) continue;
        const int created{static_cast<int>(item->coin.nHeight)};
        const bool pending_create{item->created && created > m_confirmed_height.load(std::memory_order_relaxed)};
        if (item->owed && (pending_create || (!final && created > bound_height))) {
            // As for an entry (CollectChunk()): the erase the create went over, and the item owes nothing
            // but its coin from here on.
            writes.push_back({outpoint, std::nullopt});
            ++count;
            item->owed = false;
            item->height = created;
            if (pending_create) {
                ChargeDelta(created, 1);
            } else {
                oldest_left = std::min<int>(oldest_left, created);
            }
        } else if (item->dirty && !pending_create) {
            writes.push_back({outpoint, std::make_optional(item->coin)});
            ++count;
            item->dirty = false;
            item->fresh = false;
            item->owed = false;
            // The database has the coin once the run lands, so the spend is an erase to write when the
            // block is confirmed: its marker, if the spend left none (a coin the database never held).
            int64_t added{1};
            if (it == shard.map.end()) {
                it = shard.map.try_emplace(outpoint).first;
                Resized(shard);
                Entry& entry{it->second};
                entry.height = slot.height.load(std::memory_order_relaxed);
                entry.dirty = true;
                entry.block_spend = true;
                Attach(shard, *it);
                ++added;
            }
            // The confirmed change is gone from the count, and the marker is the block's.
            slot.dirty_delta.fetch_add(added, std::memory_order_relaxed);
            // A reader that missed the outpoint in the tier may hold the base's answer from before.
            ++shard.settles;
        }
    }
    return oldest_left;
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
    // After every shard: a coin spent from an entry the run collected is in the chunk it was collected to.
    run.oldest_left = std::min<int>(run.oldest_left, CollectRecords(run.bound_height, run.final, writes, run.count));
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
    run->oldest_left = std::min<int>(run->oldest_left, CollectRecords(run->bound_height, final, run->writes, run->count));
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
    // The lists go with the entries; the few entries that stay, the open blocks' own changes, are put
    // back on theirs as the walk passes them.
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
        entry.list = LIST_NONE;
        entry.prev = nullptr;
        entry.next = nullptr;
        entry.collected = false;
        if (Pending(entry)) {
            // An open block's change: not the database's until the block is confirmed, and dirty until
            // then. The erase its create went over is the database's now (as in CollectChunk()).
            if (entry.owed) {
                add({it->first, std::nullopt});
                entry.owed = false;
                entry.height = static_cast<int>(entry.coin.nHeight);
                ChargeDelta(entry.height, 1);
            }
            Attach(shard, *it);
            ++it;
        } else {
            // As EraseEntry(), with the coin's usage taken before the coin is moved out.
            const size_t usage{entry.coin.DynamicMemoryUsage()};
            if (entry.dirty) {
                add({it->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(std::move(entry.coin))});
            }
            SubUsage(shard, usage);
            it = shard.map.erase(it);
        }
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
    // of its changes is in the database: a reader of the shard (an applier, when the flush is forced on a
    // running node) waits for the write rather than finding a coin in neither place. Every call of
    // WriteRun() names the same two heads, and a last one closes the marker. An open block's own
    // changes stay, dirty as they are; what the block changed is written: an erase owed under its create
    // with the shard, a confirmed coin it spent from its record once the shards are through.
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
    // After the shards: a coin spent since its shard was walked was read from the database.
    std::vector<CoinWrite> from_records;
    size_t record_changes{0};
    CollectRecords(head_height, /*final=*/true, from_records, record_changes);
    if (!from_records.empty()) base->WriteRun(from_records, best, wipe.old_head, /*final=*/false);
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
    // The confirmed changes the database does not have yet: what GetDirtyCount() answers.
    size_t confirmed_dirty{0};
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
            // What a block left on an entry goes when the entry is written: a clean entry is anyone's.
            assert(entry.dirty || (!entry.block_create && !entry.block_spend && !entry.owed));
            // An erase is owed under a block's create only.
            assert(!entry.owed || (entry.block_create && !entry.coin.IsSpent()));
            if (entry.dirty && (entry.owed || !Pending(entry))) ++confirmed_dirty;
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
    assert(SumTotals(&Totals::dirty) == total_dirty);
    // A confirmed coin an open block spent is in the block's record, with what the database is owed for it.
    const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
    for (const WindowBlock& slot : m_window) {
        if (slot.status.load(std::memory_order_acquire) != BlockStatus::APPLIED) continue;
        LOCK(slot.mutex);
        for (const Undo& item : slot.record) {
            if (!item.spend || item.dead) continue;
            const bool pending_create{item.created && static_cast<int>(item.coin.nHeight) > confirmed};
            if (pending_create ? item.owed : item.dirty) ++confirmed_dirty;
        }
    }
    assert(GetDirtyCount() == confirmed_dirty);
}

// ---------------------------------------------------------------------------------------------------------
// BlockApplyView

BlockApplyView::BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic)
    : CoinsViewOverlay(&store, std::move(thread_pool), deterministic), m_store{store}
{
}

BlockApplyView::~BlockApplyView()
{
    if (HasBlock()) {
        m_store.UnwindBlock(m_block);
        m_block = CoinsStore::NO_BLOCK;
    }
}

void BlockApplyView::BeginBlock(int height)
{
    Assert(!HasBlock());
    m_block = m_store.BeginBlock(height);
}

void BlockApplyView::Confirm()
{
    Assert(HasBlock());
    if (!Assume(AllInputsConsumed())) {
        LogWarning("Block %s input prevout prefetch queue was not fully consumed; inputs were accessed out of order, so prefetching degraded to serial lookups for this block.", GetBestBlock().ToString());
    }
    // The prefetch workers read the block identity (ReadBase); they are gone before it is.
    StopFetching();
    // What the block's worker has not applied yet (the tip path applies everything here).
    ApplyChanges();
    const CoinsStore::BlockId block{m_block};
    m_block = CoinsStore::NO_BLOCK;
    const int height{m_store.BlockHeight(block)};
    m_store.ConfirmBlock(block, &Pool());
    m_store.SetBestBlock(GetBestBlock(), height);
    Reset();
}

void BlockApplyView::Unwind()
{
    Assert(HasBlock());
    StopFetching(); // as in Confirm(): the workers read the identity
    const CoinsStore::BlockId block{m_block};
    m_block = CoinsStore::NO_BLOCK;
    m_store.UnwindBlock(block);
    Reset();
}

uint32_t BlockApplyView::Conflicts() const
{
    return HasBlock() ? m_store.ConflictCount(m_block) : 0;
}

void BlockApplyView::Prefetch(const CBlock& block)
{
    Assert(HasBlock());
    std::vector<COutPoint> outpoints;
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        for (const CTxIn& in : block.vtx[i]->vin) outpoints.push_back(in.prevout);
    }
    for (auto& [outpoint, coin] : m_store.FetchFor(m_block, std::move(outpoints))) {
        const size_t usage{coin.DynamicMemoryUsage()};
        if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
    }
}

void BlockApplyView::ApplyChanges()
{
    Assert(HasBlock());
    if (m_dirty_count == 0) return;
    std::vector<CoinsStore::Change> changes;
    changes.reserve(m_dirty_count);
    for (auto& [outpoint, entry] : cacheCoins) {
        if (!entry.IsDirty()) continue;
        // A dirty spent entry is the spend of a coin the base had (a fresh one spent is erased from the view
        // instead); a dirty unspent entry is a create, which overwrote a coin the base had unless fresh.
        changes.push_back({.outpoint = outpoint, .coin = std::move(entry.coin), .possible_overwrite = !entry.IsFresh()});
    }
    // The references the core held into the view are gone with the block's validation; what is applied is
    // read from the store from now on, where this block sees it. A conflict with another applied block is
    // recorded against the loser and settled at confirmation; the core is not told, so that its verdict
    // stays the block's own.
    cacheCoins.clear();
    cachedCoinsUsage = 0;
    m_dirty_count = 0;
    m_store.ApplyBatch(m_block, std::move(changes));
}

void BlockApplyView::Flush(bool /*reallocate_cache*/)
{
    Confirm();
}

std::optional<Coin> BlockApplyView::ReadBase(const COutPoint& outpoint) const
{
    return m_store.PeekCoinFor(m_block, outpoint);
}

void BlockApplyView::Reset() noexcept
{
    if (HasBlock()) {
        // The block was neither confirmed nor unwound: it failed or was abandoned.
        m_store.UnwindBlock(m_block);
        m_block = CoinsStore::NO_BLOCK;
    }
    CoinsViewOverlay::Reset();
}
