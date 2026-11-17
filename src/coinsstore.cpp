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
#include <bit>
#include <condition_variable>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32
#include <sys/mman.h>
#endif

namespace {
//! How long a waiter retries before it blocks: about what blocking and being woken would cost it.
constexpr std::chrono::microseconds SHARD_RETRY_TIME{20};
} // namespace

void CoinsStore::ShardLock::lock()
{
    if (m_mutex.try_lock()) return;
    const auto start{std::chrono::steady_clock::now()};
    do {
        std::this_thread::yield();
        if (m_mutex.try_lock()) return;
    } while (std::chrono::steady_clock::now() - start < SHARD_RETRY_TIME);
    m_mutex.lock();
}

//! The region made ready ahead (NodeRegion()) and the thread that makes it. The thread is started with the
//! first region asked for ahead and joined by the last store to be destroyed. Never destroyed itself, as
//! the nodes' depots are not.
struct CoinsStore::RegionAhead {
    Mutex mutex;
    std::condition_variable cv;
    void* ready GUARDED_BY(mutex){nullptr};
    bool wanted GUARDED_BY(mutex){false};
    //! Set by the store that found itself the last, until it has joined the thread.
    bool stop GUARDED_BY(mutex){false};
    int stores GUARDED_BY(mutex){0};
    std::thread thread GUARDED_BY(mutex);

    void Run() EXCLUSIVE_LOCKS_REQUIRED(!mutex)
    {
        util::ThreadRename("coinsahead");
        while (true) {
            {
                WAIT_LOCK(mutex, lock);
                cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(mutex) { return stop || (wanted && !ready); });
                if (stop) return;
                wanted = false;
            }
            void* next{HintedRegion(NODE_REGION_BYTES)};
            // Every small page, for where no huge page was to be had: the first touch of a huge page
            // supplies all of it, and the touches after it cost nothing.
            static constexpr size_t SMALL_PAGE_BYTES{4096};
            for (size_t at{0}; at < NODE_REGION_BYTES; at += SMALL_PAGE_BYTES) static_cast<volatile std::byte*>(next)[at] = std::byte{0};
            LOCK(mutex);
            if (stop) {
                FreeRegion(next);
                return;
            }
            ready = next;
        }
    }
};

CoinsStore::RegionAhead& CoinsStore::RegionAheadState()
{
    static RegionAhead* const ahead{new RegionAhead};
    return *ahead;
}

CoinsStore::CoinsStore(CCoinsView* base_view, size_t shards, bool deterministic)
    : CCoinsViewCache(base_view, deterministic), m_shards(std::max<size_t>(1, shards))
{
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        Resized(shard); // whatever bucket array an empty map has
    }
    RegionAhead& ahead{RegionAheadState()};
    LOCK(ahead.mutex);
    ++ahead.stores;
}

CoinsStore::~CoinsStore()
{
    RegionAhead& ahead{RegionAheadState()};
    std::thread ending;
    {
        LOCK(ahead.mutex);
        // Behind the last store nothing asks for a region ahead: its thread ends, and the region it had
        // ready goes back. (A store destroyed while an earlier last one is still at this leaves it to that one.)
        if (--ahead.stores != 0 || ahead.stop) return;
        ahead.stop = true;
        ending = std::move(ahead.thread);
    }
    ahead.cv.notify_all();
    if (ending.joinable()) ending.join();
    LOCK(ahead.mutex);
    ahead.stop = false;
    ahead.wanted = false;
    if (void* unused{std::exchange(ahead.ready, nullptr)}) FreeRegion(unused);
}

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

void* CoinsStore::HintedRegion(size_t bytes)
{
    assert(bytes != 0 && bytes % HUGE_PAGE_BYTES == 0);
    void* region{::operator new(bytes, std::align_val_t{HUGE_PAGE_BYTES})};
#ifdef MADV_HUGEPAGE
    // Advice the system is free to ignore, as it does where transparent huge pages are off.
    (void)madvise(region, bytes, MADV_HUGEPAGE);
#endif
    return region;
}

void* CoinsStore::NodeRegion()
{
    if (NodeRegionBytes() < NODE_REGION_AHEAD_FROM) return HintedRegion(NODE_REGION_BYTES);
    RegionAhead& ahead{RegionAheadState()};
    void* region;
    {
        LOCK(ahead.mutex);
        // Nodes outside the life of any store wait for their memory themselves.
        if (ahead.stores == 0 || ahead.stop) return HintedRegion(NODE_REGION_BYTES);
        if (!ahead.thread.joinable()) {
            try {
                ahead.thread = std::thread{[&ahead] { ahead.Run(); }};
            } catch (const std::system_error& e) { // what std::thread throws when no thread can be started
                // The caller is in the middle of a batch, under a shard's mutex: it waits for its region
                // itself, and the next call tries the thread again.
                LogWarning("Failed to start the coinsahead thread (%s)", e.what());
                return HintedRegion(NODE_REGION_BYTES);
            }
        }
        region = std::exchange(ahead.ready, nullptr);
        ahead.wanted = true;
    }
    ahead.cv.notify_one();
    return region ? region : HintedRegion(NODE_REGION_BYTES);
}

int64_t CoinsStore::NodeBytes()
{
    // A node's own size, not what the general allocator would hold for it: nodes are carved back to
    // back from the depot's regions, with no header and no rounding between them.
    return static_cast<int64_t>(sizeof(Map::Cell));
}

size_t CoinsStore::RegionAheadBytes() const
{
    return SumTotals(&Totals::entries) * static_cast<size_t>(NodeBytes()) >= NODE_REGION_AHEAD_FROM ? NODE_REGION_BYTES : 0;
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
    MyTotals().bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::SubUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::Resized(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    // A cell for each entry and the bucket array.
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
    return NodeBytes() + static_cast<int64_t>(entry.coin.DynamicMemoryUsage());
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
        Assert(shard.dirty_count > 0);
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
    Assert(!it->second.collected);
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
    Assert(!entry.collected); // as in EraseDetached()
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
    // The entry's state says what there is to see, the heights say whether this viewer sees it.
    switch (coins_entry::ReadingOf(StateOf(entry))) {
    case coins_entry::Reading::NOTHING: return Sight::NOTHING;
    case coins_entry::Reading::COIN: return Sight::COIN;
    case coins_entry::Reading::BLOCK_RECORD:
        // The viewer's own spend, or a lower block's: spent.
        if (entry.height <= viewer_height) return Sight::NOTHING;
        return entry.height > m_confirmed_height.load(std::memory_order_relaxed) ? Sight::RECORD : Sight::NOTHING;
    case coins_entry::Reading::BLOCK_COIN: {
        const int created{static_cast<int>(entry.coin.nHeight)};
        if (created <= viewer_height) return Sight::COIN;
        return created > m_confirmed_height.load(std::memory_order_relaxed) ? Sight::NOTHING : Sight::COIN;
    }
    } // no default case, so the compiler can warn about missing cases
    assert(false);
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

//! The last spend of `outpoint` in a block's record that still stands, or nullptr; with the block's mutex
//! held.
template <typename Record>
static auto* LiveSpend(Record& record, const COutPoint& outpoint)
{
    const auto item{std::find_if(record.rbegin(), record.rend(), [&](const auto& u) { return u.spend && !u.dead && u.outpoint == outpoint; })};
    return item == record.rend() ? nullptr : &*item;
}

std::optional<Coin> CoinsStore::RecordCoin(int spender_height, const COutPoint& outpoint, int viewer_height) const
{
    const BlockId id{SlotAt(spender_height)};
    if (id == NO_BLOCK) return std::nullopt;
    const WindowBlock& slot{m_window[id]};
    LOCK(slot.mutex);
    if (slot.status.load(std::memory_order_relaxed) == BlockStatus::FREE || slot.height.load(std::memory_order_relaxed) != spender_height) return std::nullopt;
    const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
    const auto* item{LiveSpend(slot.record, outpoint)};
    if (!item) return std::nullopt;
    // A coin an open block created, which is not there yet from where the viewer stands.
    const int created{static_cast<int>(item->coin.nHeight)};
    if (item->created && created > confirmed && created > viewer_height) return std::nullopt;
    return item->coin;
}

std::optional<Coin> CoinsStore::RecordScan(const COutPoint& outpoint) const
{
    const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
    for (BlockId id{1}; id < m_window.size(); ++id) {
        const WindowBlock& slot{m_window[id]};
        if (slot.status.load(std::memory_order_acquire) == BlockStatus::FREE) continue;
        LOCK(slot.mutex);
        if (slot.status.load(std::memory_order_relaxed) == BlockStatus::FREE) continue;
        if (const auto* item{LiveSpend(slot.record, outpoint)}) {
            // A coin an open block created is no reader's.
            if (item->created && static_cast<int>(item->coin.nHeight) > confirmed) return std::nullopt;
            return item->coin;
        }
    }
    return std::nullopt;
}

std::optional<Coin> CoinsStore::OwedValue(int creator_height, const COutPoint& outpoint) const
{
    const BlockId id{SlotAt(creator_height)};
    if (id == NO_BLOCK) return std::nullopt;
    const WindowBlock& slot{m_window[id]};
    LOCK(slot.mutex);
    if (slot.status.load(std::memory_order_relaxed) == BlockStatus::FREE || slot.height.load(std::memory_order_relaxed) != creator_height) return std::nullopt;
    for (auto it{slot.record.rbegin()}; it != slot.record.rend(); ++it) {
        if (it->spend || it->dead || it->outpoint != outpoint) continue;
        if (!it->coin.IsSpent()) return it->coin;
        break;
    }
    return std::nullopt;
}

void CoinsStore::Record(WindowBlock& mine, std::vector<Undo>& undo, int64_t& delta)
{
    if (delta != 0) mine.dirty_delta.fetch_add(std::exchange(delta, 0), std::memory_order_relaxed);
    if (undo.empty()) return;
    LOCK(mine.mutex);
    for (const Undo& item : undo) {
        if (item.spend && item.owed) mine.owed_spends = true;
    }
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
        Transit(it->second, coins_entry::ABSENT, Event::BaseCopy, static_cast<int>(coin->nHeight));
        AddUsage(shard, it->second.coin.DynamicMemoryUsage());
        Attach(shard, *it);
        return coin;
    }
}

std::optional<Coin> CoinsStore::PeekCoinFor(BlockId viewer, const COutPoint& outpoint) const
{
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
    // A copy, good until the thread's next call: a reference into a shard would not do (see the class).
    std::optional<Coin> coin{Lookup(outpoint)};
    if (!coin) return STORE_EMPTY_COIN;
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
void CoinsStore::KeepWritten(std::vector<CoinWrite> restored)
{
    for (const CoinWrite& write : restored) {
        Shard& shard{ShardFor(write.outpoint)};
        LOCK(shard.mutex);
        if (auto it{shard.map.find(write.outpoint)}; it != shard.map.end()) Transit(it->second, Event::KeptWritten);
    }
    m_restored.insert(m_restored.end(), std::make_move_iterator(restored.begin()), std::make_move_iterator(restored.end()));
}

// Confirmed changes without a block identity
//
// None is made while a block is open: such a change clears the block flags of an entry an open block
// created or spent (coins_entry_machine.h), and the block's unwind would then skip the entry. The callers
// keep to that (a block is disconnected, and a view written through, before any block is staged); each
// entry point checks it.

void CoinsStore::AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    Assert(m_open_slots.load(std::memory_order_relaxed) == 0); // see "Confirmed changes without a block identity"
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    Entry& entry{it->second};
    const State from{StateOf(entry)};
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
    Transit(entry, from, fresh ? Event::ConfirmedNew : Event::ConfirmedWrite, TagFor(entry.coin));
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    Attach(shard, *it);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Assert(m_open_slots.load(std::memory_order_relaxed) == 0); // see "Confirmed changes without a block identity"
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const size_t usage{coin.DynamicMemoryUsage()};
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    if (!inserted) return;
    Entry& entry{it->second};
    entry.coin = std::move(coin);
    Transit(entry, coins_entry::ABSENT, entry.coin.IsSpent() ? Event::ConfirmedSpend : Event::ConfirmedWrite, m_change_height);
    AddUsage(shard, usage);
    Attach(shard, *it);
}

bool CoinsStore::SpendEntry(Shard& shard, Map::iterator it, Coin* moveto)
{
    AssertLockHeld(shard.mutex);
    // As CCoinsViewCache::SpendCoin: an entry that is present is "spent" whether or not it still holds a
    // coin, so that a caller spending an already spent entry gets the same answer from both.
    Entry& entry{it->second};
    const State from{StateOf(entry)};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    if (moveto) *moveto = std::move(entry.coin);
    if (entry.fresh) {
        // Never written: the database has nothing to erase.
        EraseDetached(shard, it);
        return true;
    }
    entry.coin.Clear();
    // Under the change height, or where an earlier change is owed still (coins_entry::Height::Lower).
    Transit(entry, from, Event::ConfirmedSpend, m_change_height);
    Attach(shard, *it);
    return true;
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Assert(m_open_slots.load(std::memory_order_relaxed) == 0); // see "Confirmed changes without a block identity"
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
    TraceUncache(outpoint, it->second.coin);
    EraseEntry(shard, it);
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    Assert(m_open_slots.load(std::memory_order_relaxed) == 0); // see "Confirmed changes without a block identity"
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) continue;
        const COutPoint& outpoint{it->first};
        Shard& shard{ShardFor(outpoint)};
        LOCK(shard.mutex);
        auto [ours, inserted]{shard.map.try_emplace(outpoint)};
        if (inserted) Resized(shard);
        Entry& entry{ours->second};
        const State from{StateOf(entry)};
        Event event;
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                // Created and spent inside the child: nothing to record.
                EraseDetached(shard, ours);
                continue;
            }
            entry.coin = cursor.WillErase(*it) ? std::move(it->second.coin) : it->second.coin;
            event = entry.coin.IsSpent() ? Event::ConfirmedSpend : it->second.IsFresh() ? Event::ConfirmedNew : Event::ConfirmedWrite;
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
            // Not made fresh: the entry may be in the database already (see CCoinsViewCache::BatchWrite).
            event = entry.coin.IsSpent() ? Event::ConfirmedSpend : Event::BatchOverwrite;
        }
        Transit(entry, from, event, entry.coin.IsSpent() ? m_change_height : TagFor(entry.coin));
        AddUsage(shard, entry.coin.DynamicMemoryUsage());
        Attach(shard, *ours);
    }
    SetBestBlock(block_hash);
}

// ---------------------------------------------------------------------------------------------------------
// Changes with a block identity

CoinsStore::BlockId CoinsStore::BeginBlock(int height, bool mark_fresh_spends)
{
    for (BlockId id{1}; id < m_window.size(); ++id) {
        WindowBlock& slot{m_window[id]};
        if (slot.status.load(std::memory_order_relaxed) != BlockStatus::FREE) continue;
        // Counted before the slot shows: a reader that finds the count at zero has no record to look into.
        m_open_slots.fetch_add(1, std::memory_order_relaxed);
        LOCK(slot.mutex);
        slot.conflicts = 0;
        slot.owed_spends = false;
        slot.record.clear();
        slot.dirty_delta.store(0, std::memory_order_relaxed);
        slot.height.store(height, std::memory_order_relaxed);
        slot.begin_confirmed.store(m_confirmed_height.load(std::memory_order_relaxed), std::memory_order_relaxed);
        slot.mark_fresh_spends.store(mark_fresh_spends, std::memory_order_relaxed);
        slot.creates_pending.store(false, std::memory_order_relaxed);
        slot.status.store(BlockStatus::APPLIED, std::memory_order_release);
        return id;
    }
    // More blocks open than the widest window can hold: a caller's error, never the chain's.
    assert(false);
    return NO_BLOCK;
}

int CoinsStore::BlockHeight(BlockId block) const
{
    Assert(block < m_window.size());
    return m_window[block].height.load(std::memory_order_relaxed);
}

//! An entry of the pending transaction ids: the id's tag, the announcing block's height and its slot.
static constexpr uint64_t PENDING_HEIGHT_MASK{0xffffff};
static size_t PendingTxidPlace(const Txid& txid, size_t places) { return txid.ToUint256().GetUint64(1) & (places - 1); }
static uint64_t PendingTxidTag(const Txid& txid) { return txid.ToUint256().GetUint64(2) >> 32; }

void CoinsStore::AnnounceCreates(BlockId block, std::span<const CTransactionRef> txs)
{
    Assert(block != NO_BLOCK && block < m_window.size());
    static_assert(MAX_WINDOW_SLOTS <= 256 && std::has_single_bit(PENDING_TXID_PLACES));
    WindowBlock& slot{m_window[block]};
    std::atomic<uint64_t>* table{m_pending_txids.load(std::memory_order_acquire)};
    if (!table) {
        std::call_once(m_pending_txids_once, [&] {
            m_pending_txids_owner = std::make_unique<std::atomic<uint64_t>[]>(PENDING_TXID_PLACES);
            m_pending_txids.store(m_pending_txids_owner.get(), std::memory_order_release);
        });
        table = m_pending_txids.load(std::memory_order_acquire);
    }
    const uint64_t owner{(static_cast<uint64_t>(slot.height.load(std::memory_order_relaxed)) & PENDING_HEIGHT_MASK) << 8 | block};
    for (const CTransactionRef& tx : txs) {
        table[PendingTxidPlace(tx->GetHash(), PENDING_TXID_PLACES)].store(PendingTxidTag(tx->GetHash()) << 32 | owner, std::memory_order_relaxed);
    }
    slot.creates_pending.store(true, std::memory_order_release);
}

void CoinsStore::CreatesApplied(BlockId block)
{
    Assert(block != NO_BLOCK && block < m_window.size());
    m_window[block].creates_pending.store(false, std::memory_order_release);
}

bool CoinsStore::CreatesPendingBelow(BlockId viewer, const Txid& txid) const
{
    const std::atomic<uint64_t>* table{m_pending_txids.load(std::memory_order_acquire)};
    if (!table || viewer == NO_BLOCK) return false;
    const uint64_t entry{table[PendingTxidPlace(txid, PENDING_TXID_PLACES)].load(std::memory_order_relaxed)};
    if (entry >> 32 != PendingTxidTag(txid)) return false;
    const BlockId maker{static_cast<BlockId>(entry & 0xff)};
    if (maker == NO_BLOCK || maker >= m_window.size()) return false;
    // The entry is that of a block still open at the height it was announced at, below the viewer's, whose
    // changes are not in the store. A slot freed and claimed again has had its mark cleared.
    const WindowBlock& slot{m_window[maker]};
    if (!slot.creates_pending.load(std::memory_order_acquire)) return false;
    if (slot.status.load(std::memory_order_acquire) != BlockStatus::APPLIED) return false;
    const int height{slot.height.load(std::memory_order_relaxed)};
    return (static_cast<uint64_t>(height) & PENDING_HEIGHT_MASK) == (entry >> 8 & PENDING_HEIGHT_MASK) && height < BlockHeight(viewer);
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

bool CoinsStore::CreateLocked(Shard& shard, int height, const COutPoint& outpoint, uint64_t hash, Coin&& coin, bool possible_overwrite, bool must_be_new, bool base_unseen, std::vector<Undo>& undo, int64_t& delta)
{
    AssertLockHeld(shard.mutex);
    // The coin's height is what tells the create from a confirmed one (Pending()).
    Assume(static_cast<int>(coin.nHeight) == height);
    // No entry is no proof of no coin where changes confirmed since the block was opened have been written:
    // a coin a lower block made here after this block's check may have left the tier for the database
    // (EvictClean()). The block is connected again alone, where its check reads the database.
    if (must_be_new && base_unseen && shard.map.find(outpoint, hash) == shard.map.end()) return false;
    auto [it, inserted]{shard.map.try_emplace(outpoint, hash)};
    Entry& entry{it->second};
    // What the block's unwind puts back: nothing, or the confirmed coin this create overwrites.
    Undo item{.coin = {}, .outpoint = outpoint};
    const State from{StateOf(entry)};
    Event event;
    if (inserted) {
        Resized(shard);
        event = possible_overwrite ? Event::BlockCreateBlind : Event::BlockCreateFresh;
        ++delta;
    } else {
        const bool pending{Pending(entry)};
        // An open block's spend under a create is an outpoint made twice (BIP30), and so is a lower
        // open block's create: not this block's to settle here. It fails, and is connected again alone.
        // A higher block's spend comes first: its coin, which this block sees, is no defect of the caller.
        if (pending && entry.coin.IsSpent()) return false;
        // A coin this block sees where it took none to be: a lower block made the outpoint after this
        // block looked (it was open then, or is still), which this block's validation could not know.
        // Nothing is overwritten on that ground; the block is connected again alone, where the check that
        // rejects it on its own reads the coin. A create that may overwrite and was not checked for an
        // earlier coin (`must_be_new` false: BIP30 is not enforced at its height) overwrites.
        if ((must_be_new || !possible_overwrite) && See(entry, height) != Sight::NOTHING) return false;
        if (pending) {
            const int other{static_cast<int>(entry.coin.nHeight)};
            if (other < height) return false;
            event = Event::BlockCreateTakeover;
            if (other > height) {
                // A higher block created this outpoint first: a duplicate it could not see, since this
                // block had not produced the original yet. The higher block gets the conflict: it is not
                // confirmed from this pass but connected again alone, in order, where it passes or fails as
                // it would have. Its entry is this block's now, with what it added to the dirty entries.
                const int64_t moved{entry.owed ? 0 : 1};
                if (const BlockId id{SlotAt(other)}; id != NO_BLOCK) {
                    WindowBlock& charged{m_window[id]};
                    LOCK(charged.mutex);
                    // Unless that create overwrote a confirmed coin, kept in its record for its unwind (a
                    // coinbase at the chain's BIP30 duplicates): this block cannot see that coin and would
                    // take the entry over without it. It fails instead, and is connected again alone once the
                    // higher block is unwound and the coin back, where its own checks read the coin.
                    if (std::any_of(charged.record.rbegin(), charged.record.rend(), [&](const Undo& u) {
                            return !u.spend && !u.dead && u.outpoint == outpoint && !u.coin.IsSpent();
                        })) {
                        return false;
                    }
                    if (charged.status.load(std::memory_order_relaxed) == BlockStatus::APPLIED) ++charged.conflicts;
                    charged.dirty_delta.fetch_sub(moved, std::memory_order_relaxed);
                }
                delta += moved;
                if (possible_overwrite) event = Event::BlockCreateTakeoverBlind;
            }
            // Under an erase still owed the entry keeps that change's height, as below (the rule's Lower).
        } else {
            // A confirmed entry. Dirty, it records a change the database does not have yet, which is owed
            // under the create while the block is open and if it is unwound (Entry::owed): the entry stays
            // at the height of that change. What is owed is an erase, except where the entry is a dirty
            // unspent coin (a coinbase at the chain's two BIP30 duplicates, over a coin the database does
            // not have yet): the create's record keeps that coin for the block's unwind and for a run that
            // passes while the block is open.
            if (!entry.coin.IsSpent()) {
                item.coin = entry.coin;
                item.height = entry.height;
                item.dirty = entry.dirty;
            }
            event = Event::BlockCreateOnConfirmed;
            // What a dirty entry records is owed under the create, and the entry stays where that is filed.
            if (!entry.dirty) ++delta;
        }
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
    }
    entry.coin = std::move(coin);
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    // Dirty from now on, where the write-out will find it once the block is confirmed; it passes over the
    // entry until then (Pending()).
    Transit(entry, from, event, height);
    Attach(shard, *it);
    undo.push_back(std::move(item));
    return true;
}

//! Whether `coin` is the one a block validated its spend against. Two coins of an outpoint are told
//! apart by where they were made: the outputs of a transaction made twice are the same.
static bool IsCoin(const Coin& coin, const Coin& known)
{
    return coin.nHeight == known.nHeight && coin.fCoinBase == known.fCoinBase;
}

CoinsStore::SpendResult CoinsStore::SpendLocked(Shard& shard, int height, bool mark_fresh, const COutPoint& outpoint, uint64_t hash, const Coin* from_base, const Coin* known, std::vector<Undo>& undo, int64_t& delta)
{
    AssertLockHeld(shard.mutex);
    auto it{shard.map.find(outpoint, hash)};
    if (it == shard.map.end()) {
        // Not in the tier (the block's read left nothing behind, as at the tip, or what it brought in was
        // evicted since): the caller reads the base with the shard unlocked. The database's coin goes to
        // the record, and the tier keeps the spend, for the erase.
        if (!from_base) return SpendResult::MISS;
        if (known && !IsCoin(*from_base, *known)) return SpendResult::CONFLICT;
        it = shard.map.try_emplace(outpoint, hash).first;
        Resized(shard);
        Entry& marker{it->second};
        Transit(marker, coins_entry::ABSENT, Event::BlockSpendMarker, height);
        Attach(shard, *it);
        ++delta;
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
            Undo* item{LiveSpend(theirs.record, outpoint)};
            if (!item) return SpendResult::CONFLICT;
            // A coin a block above this one created is not there yet.
            const int created{static_cast<int>(item->coin.nHeight)};
            if (item->created && created > height && created > m_confirmed_height.load(std::memory_order_relaxed)) return SpendResult::CONFLICT;
            if (known && !IsCoin(item->coin, *known)) return SpendResult::CONFLICT;
            if (status == BlockStatus::APPLIED) ++theirs.conflicts;
            const int64_t moved{item->dirty ? 0 : 1};
            theirs.dirty_delta.fetch_sub(moved, std::memory_order_relaxed);
            delta += moved;
            undo.push_back(std::move(*item));
            item->dead = true;
        }
        Detach(shard, *it);
        // The same spend of the same coin, taken over: what it says of the database stays true.
        Transit(entry, Event::BlockSpendTakeover, height);
        Attach(shard, *it);
        return SpendResult::DONE;
    }
    case Sight::COIN: break;
    }
    // Another coin than the block validated: lower blocks spent that one and made the outpoint again
    // after this block looked (a transaction made twice). The verdict on the spend, the maturity of a
    // coinbase for one, was on the other coin, so the block is connected again alone.
    if (known && !IsCoin(entry.coin, *known)) return SpendResult::CONFLICT;
    Undo item{.coin = {}, .outpoint = outpoint, .height = entry.height, .spend = true, .dirty = entry.dirty, .fresh = entry.fresh, .created = entry.block_create, .owed = entry.owed};
    const State from{StateOf(entry)};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    item.coin = std::move(entry.coin);
    if (entry.fresh && !mark_fresh) {
        // Never written: the database has nothing to erase and the tier nothing to keep. The coin is in
        // the record, for the unwind and for whoever is still to see it.
        EraseDetached(shard, it);
        --delta;
    } else {
        // The marker of a never-written coin (BeginBlock()) is as any other to the open blocks: the record
        // has the flags to put back. To a run it is nothing to write (Entry::unwritten).
        entry.coin.Clear();
        if (!entry.dirty) ++delta;
        Transit(entry, from, Event::BlockSpend, height);
        Attach(shard, *it);
    }
    undo.push_back(std::move(item));
    return SpendResult::DONE;
}

bool CoinsStore::ApplySpend(BlockId block, const COutPoint& outpoint, const Coin* known)
{
    assert(block != NO_BLOCK);
    const int height{BlockHeight(block)};
    Shard& shard{ShardFor(outpoint)};
    WindowBlock& mine{m_window[block]};
    const bool mark_fresh{mine.mark_fresh_spends.load(std::memory_order_relaxed)};
    std::vector<Undo> undo;
    int64_t delta{0};
    std::optional<Coin> from_base;
    uint64_t settles{0};
    const uint64_t hash{KeyHash(outpoint)};
    while (true) {
        SpendResult result;
        {
            LOCK(shard.mutex);
            // A write-out settled entries of the shard while the base was read (as in LookupFor()): the
            // coin read may be stale, and inserting it would hide a double spend. Read again.
            if (from_base && shard.settles != settles) from_base.reset();
            result = SpendLocked(shard, height, mark_fresh, outpoint, hash, from_base ? &*from_base : nullptr, known, undo, delta);
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
    const int height{BlockHeight(block)};
    // Grouped by shard, so that each shard is taken once.
    std::vector<Change*> applied;
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    applied.reserve(changes.size());
    shard_of.reserve(changes.size());
    hashes.reserve(changes.size());
    for (Change& change : changes) {
        if (!change.coin.IsSpent() && change.coin.out.scriptPubKey.IsUnspendable()) continue;
        applied.push_back(&change);
        shard_of.push_back(ShardIndex(change.outpoint));
        hashes.push_back(KeyHash(change.outpoint));
    }
    WindowBlock& mine{m_window[block]};
    const bool mark_fresh{mine.mark_fresh_spends.load(std::memory_order_relaxed)};
    const int begin_confirmed{mine.begin_confirmed.load(std::memory_order_relaxed)};
    {
        LOCK(mine.mutex);
        mine.record.reserve(mine.record.size() + applied.size());
    }
    // A shard's changes, until they are in the record.
    std::vector<Undo> undo;
    int64_t delta{0};
    uint32_t conflicts{0};
    std::vector<const Change*> misses;
    VisitShards(block, shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        // A create looks for its outpoint only to find none.
        shard.map.PrefetchAll(hashes, items, [&](uint32_t i) { return applied[i]->coin.IsSpent(); });
        // Read with the shard held: an entry dropped from it was dropped after the height was stored.
        const bool base_unseen{m_dropped_through.load(std::memory_order_relaxed) > begin_confirmed};
        for (const uint32_t i : items) {
            Change* change{applied[i]};
            if (change->coin.IsSpent()) {
                switch (SpendLocked(shard, height, mark_fresh, change->outpoint, hashes[i], nullptr, change->known_coin ? &change->coin : nullptr, undo, delta)) {
                case SpendResult::DONE: break;
                case SpendResult::CONFLICT: ++conflicts; break;
                case SpendResult::MISS: misses.push_back(change); break;
                }
            } else if (!CreateLocked(shard, height, change->outpoint, hashes[i], std::move(change->coin), change->possible_overwrite, change->must_be_new, base_unseen, undo, delta)) {
                ++conflicts;
            }
        }
        Record(mine, undo, delta);
    });
    if (conflicts != 0) {
        LOCK(mine.mutex);
        mine.conflicts += conflicts;
    }
    // Spends of coins the tier does not hold: one by one, via the base. For a window block these are the
    // few evicted since its lookups (BlockApplyView::SetKeepBaseReads()).
    for (const Change* change : misses) ApplySpend(block, change->outpoint, change->known_coin ? &change->coin : nullptr);
}

std::vector<std::pair<COutPoint, Coin>> CoinsStore::FetchFor(BlockId viewer, std::vector<COutPoint>&& outpoints) const
{
    const int viewer_height{ViewerHeight(viewer)};
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    shard_of.reserve(outpoints.size());
    hashes.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) {
        shard_of.push_back(ShardIndex(outpoint));
        hashes.push_back(KeyHash(outpoint));
    }
    std::vector<std::pair<COutPoint, Coin>> found;
    found.reserve(outpoints.size());
    VisitShards(viewer, shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        shard.map.PrefetchAll(hashes, items, [](uint32_t) { return true; });
        for (const uint32_t i : items) {
            const COutPoint& outpoint{outpoints[i]};
            const auto it{shard.map.find(outpoint, hashes[i])};
            if (it == shard.map.end() || See(it->second, viewer_height) != Sight::COIN) continue;
            Reference(it->second);
            found.emplace_back(outpoint, it->second.coin);
        }
    });
    return found;
}

void CoinsStore::ConfirmBlock(BlockId block)
{
    assert(block != NO_BLOCK);
    Assert(block < m_window.size());
    WindowBlock& slot{m_window[block]};
    std::vector<Undo> record;
    const int height{slot.height.load(std::memory_order_relaxed)};
    // From the lowest up: a block confirmed after a higher one had its changes read as confirmed while it
    // was open.
    Assert(height > m_confirmed_height.load(std::memory_order_relaxed));
    const bool marked{slot.mark_fresh_spends.load(std::memory_order_relaxed)};
    bool owed_spends{false};
    {
        LOCK(slot.mutex);
        Assert(slot.status.load(std::memory_order_relaxed) == BlockStatus::APPLIED);
        owed_spends = std::exchange(slot.owed_spends, false);
        // All there is to it: from here on the block's creates and spent markers read as anyone's
        // (Pending(), See()). They are dirty already, on the lists of the block's height.
        m_confirmed_height.store(slot.height.load(std::memory_order_relaxed), std::memory_order_release);
        record.swap(slot.record);
        slot.conflicts = 0;
        slot.dirty_delta.store(0, std::memory_order_relaxed);
        slot.status.store(BlockStatus::FREE, std::memory_order_release);
    }
    m_open_slots.fetch_sub(1, std::memory_order_relaxed);
    if (marked) {
        // The markers of coins the database never held have no one left to tell: freed here, they do not
        // wait in the map for a run that a large cache never makes. A run may have met one first.
        for (const Undo& item : record) {
            if (!item.spend || !item.fresh || item.dead) continue;
            Shard& shard{ShardFor(item.outpoint)};
            LOCK(shard.mutex);
            const auto it{shard.map.find(item.outpoint)};
            if (it == shard.map.end()) continue;
            const Entry& entry{it->second};
            if (entry.unwritten && entry.height == height) EraseEntry(shard, it);
        }
    }
    if (owed_spends) {
        // A spend that took an entry with an erase still owed left the entry under the block's height. The
        // erase is owed since the earlier change: the entry goes back under that height
        // (coins_entry::Event::BlockSpendConfirmed), so that a run bounded between the two heights writes
        // the erase. A higher block may have made the outpoint again meanwhile, over the same erase. A run
        // that collected the entry under the block's height writes the erase itself.
        for (const Undo& item : record) {
            if (!item.spend || !item.owed || item.dead || item.height >= height) continue;
            Shard& shard{ShardFor(item.outpoint)};
            LOCK(shard.mutex);
            const auto it{shard.map.find(item.outpoint)};
            if (it == shard.map.end()) continue;
            Entry& entry{it->second};
            if (entry.height != height || entry.collected || !(entry.owed || entry.coin.IsSpent())) continue;
            Detach(shard, *it);
            Transit(entry, Event::BlockSpendConfirmed, item.height);
            Attach(shard, *it);
        }
    }
    // The record is freed as it goes out of scope here, outside the slot's mutex.
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
        if (!item.coin.IsSpent() && item.dirty) {
            // The create overwrote a confirmed coin the database did not have yet (CreateLocked()): put back
            // as it was, dirty, also where a run wrote the erase owed under the create meanwhile (it cleared
            // `owed`): the next run writes the coin again. A confirmed coin the database had (not dirty) owes
            // nothing and goes with the entry below: the database still has it.
            Detach(shard, *it);
            SubUsage(shard, entry.coin.DynamicMemoryUsage());
            entry.coin = std::move(item.coin);
            AddUsage(shard, entry.coin.DynamicMemoryUsage());
            Transit(entry, Event::UnwindCreateToCoin, item.height);
            Attach(shard, *it);
            return;
        }
        if (!entry.owed) {
            EraseEntry(shard, it);
            return;
        }
        // The erase the create went over is owed still: a spent entry again, at the height it kept.
        const State from{StateOf(entry)};
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
        entry.coin.Clear();
        Transit(entry, from, Event::UnwindCreateToErase);
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
        const State from{StateOf(entry)};
        entry.coin = std::move(item.coin);
        AddUsage(shard, entry.coin.DynamicMemoryUsage());
        // As the record has it. Clean, the coin has been written since (CollectRecords()) and is anyone's.
        using namespace coins_entry;
        const State recorded{static_cast<State>((item.dirty ? DIRTY : 0) | (item.fresh ? FRESH : 0) | (item.created && item.dirty ? BLOCK_CREATE : 0) | (item.owed ? OWED : 0))};
        Enter(entry, NextUnwindSpendRestore(from, recorded), NextHeight(UNWIND_SPEND_RESTORE_HEIGHT, from, entry.height, item.height));
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
        Transit(entry, Event::UnwindSpendToErase, item.height);
    } else {
        restore(entry);
    }
    Attach(shard, *it);
}

void CoinsStore::UnwindBlock(BlockId block)
{
    assert(block != NO_BLOCK);
    Assert(block < m_window.size());
    WindowBlock& slot{m_window[block]};
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
    // The sum kept as the entries change (Totals), and the region held ahead of a large store's nodes.
    return SumTotals(&Totals::bytes) + RegionAheadBytes();
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
    // Round the shards, a batch from the old end of each one's clean list at a time, so that the shards
    // age together and no shard's lock is held for long. An entry with its mark set goes to the young end.
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
    // Changes made below the written head (a disconnect's) are tagged with the fork height while the
    // database holds the state of a block above it. Stopping at a bound would write some of them under a
    // marker whose replay starts from the written head, which restores nothing of what they took.
    final = final || m_fell_below_written;
    auto run{std::make_unique<WriteOutRun>()};
    run->restored = TakeRestored();
    run->new_head = new_head;
    run->old_head = m_written_head;
    run->bound_height = final ? head_height : bound_height;
    run->head_height = head_height;
    run->final = final;
    // Until every shard has been collected the bound stands in for the settled height (TagFor()).
    m_inflight_settled_height = run->bound_height;
    m_run_in_flight = true;
    // A final run carries every change made so far; SetTierHeight() notes a disconnect below its head.
    if (final) m_fell_below_written = false;
    // The highest height a run may have written (see m_dropped_through): a run bounded lower than an earlier
    // one does not take it back. Only a disconnect lowers it (SetTierHeight()).
    m_dropped_through.store(std::max(m_dropped_through.load(std::memory_order_relaxed), run->bound_height), std::memory_order_relaxed);
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
 * Put the changes from `from` on in the order of the database's keys, descending: by the leading bytes
 * of the txid and, where they are equal, by the outpoint (the order of the keys, but among the outputs
 * past the 16511th of one transaction, whose index is a byte longer in the key). The database finds the
 * place of a key next to the last one it took faster than the place of a key anywhere, and a batch it
 * cuts at its batch size (CCoinsViewDB::WriteRun()) is then cut along its keys.
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
    std::sort(sorter.order.begin(), sorter.order.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return writes[from + b.second].outpoint < writes[from + a.second].outpoint;
    });
    sorter.sorted.clear();
    sorter.sorted.reserve(count);
    for (const auto& [prefix, i] : sorter.order) sorter.sorted.push_back(std::move(writes[from + i]));
    std::move(sorter.sorted.begin(), sorter.sorted.end(), writes.begin() + from);
    sorter.sorted.clear();
}

void CoinsStore::CollectChunk(Shard& shard, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<Node*>& nodes) const
{
    AssertLockHeld(shard.mutex);
    // By index, and with no reference to a list kept across an entry: moving an owed entry to its coin's
    // span (below) can free the lists with the shard's last dirty entry and allocate them again, longer.
    for (size_t span{0}; span < shard.dirty_spans.size(); ++span) {
        // The oldest change this list keeps for a later run.
        int oldest_left{std::numeric_limits<int>::max()};
        for (Node* node{shard.dirty_spans[span].head}, *next{nullptr}; node; node = next) {
            next = node->second.next;
            Entry& entry{node->second};
            // An open block's change is passed over until the block is confirmed. What the block changed
            // is not: a confirmed coin it spent is in its record (CollectRecords()), and the change its
            // create went over is owed by the entry, which kept that change's height.
            const bool pending{Pending(entry)};
            if (pending && !entry.owed) continue;
            if (!run.final && entry.height > run.bound_height) {
                oldest_left = std::min<int>(oldest_left, entry.height);
                continue;
            }
            if (size >= limit) return;
            if (const int created{static_cast<int>(entry.coin.nHeight)}; entry.owed && (pending || (!run.final && created > run.bound_height))) {
                // The create is not the run's to write, the change under it is (OwedValue()): left behind,
                // it would be missing from a database whose marker closes at the run's head. Written, the
                // entry owes nothing but its coin, at the coin's height.
                writes.push_back({node->first, pending ? OwedValue(created, node->first) : std::nullopt});
                size += BatchBytes(writes.back());
                ++run.count;
                Detach(shard, *node);
                Transit(entry, Event::RunWritesOwed, created);
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
            if (entry.unwritten) {
                // The marker of a coin the database never held, its block confirmed and ConfirmBlock() not
                // yet here: nothing to erase there, and nothing left to say here. The lists go with the
                // shard's last dirty entry.
                EraseEntry(shard, *node);
                if (shard.dirty_count == 0) return;
                continue;
            }
            writes.push_back({node->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(entry.coin)});
            nodes.push_back(node);
            size += BatchBytes(writes.back());
            ++run.count;
            run.bytes += static_cast<size_t>(DirtyBytes(entry));
            // Not fresh from here on (StreamWriteOut()).
            Transit(entry, Event::RunCollects);
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
    Detach(shard, node);
    // Clean, and what the database holds is anyone's.
    Transit(entry, Event::RunLands);
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
            item = LiveSpend(slot.record, outpoint);
        }
        if (!item) continue;
        const int created{static_cast<int>(item->coin.nHeight)};
        const bool pending_create{item->created && created > m_confirmed_height.load(std::memory_order_relaxed)};
        if (item->owed && (pending_create || (!final && created > bound_height))) {
            // The erase the create went over, and the item owes nothing but its coin from here on. An entry
            // (CollectChunk()) writes the owed value, which may be a coin the create went over. An open
            // block's create that an item spends is never such a one: only a coinbase goes over a coin, and
            // none is spent within a window, which is shorter than a coinbase takes to mature. A confirmed
            // create above the bound of a run that is not final can be one: the erase is then written in
            // place of the coin it went over, the case BeginWriteOut() names (less than the state below
            // the create, never a coin that state does not have).
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
                Transit(entry, coins_entry::ABSENT, Event::BlockSpendMarker, slot.height.load(std::memory_order_relaxed));
                Attach(shard, *it);
                ++added;
            } else {
                // The database has the coin now, whatever the marker said.
                Transit(it->second, Event::RunWritesRecord);
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
    // At least one change to a chunk, whatever the limit.
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
        // Under the run's two heads (BeginWriteOut()).
        base->WriteRun(writes, run.new_head, run.old_head, /*final=*/false);
        ++run.chunks;
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
    // From the highest shard down, for the reason given in Flush().
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
    // that the old head recorded there is never below what pruning goes by.
    run.settled_head = block_at(run.settled_height);
    base->WriteRun({}, run.new_head, run.settled_head, /*final=*/false);
}

void CoinsStore::CommitWriteOut(const WriteOutRun& run)
{
    Assert(m_run_in_flight);
    m_dropped_through.store(std::max({m_dropped_through.load(std::memory_order_relaxed), run.bound_height, run.settled_height}), std::memory_order_relaxed);
    Assert(run.final || !run.settled_head.IsNull());
    m_written_head = run.final ? run.new_head : run.settled_head;
    m_written_height = run.settled_height;
    m_run_in_flight = false;
}

void CoinsStore::AbandonWriteOut()
{
    // What the run was to carry is unwritten still, a final run's changes from below the written head too.
    m_fell_below_written = true;
    m_run_in_flight = false;
    // Nothing may close the run's marker: no run and no flush follows, and the next start replays from the
    // old head.
    m_flush_failed = true;
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

//! The most workers a flush has. The database takes one batch at a time: these keep it busy, and more
//! would wait for it.
static constexpr size_t MAX_WIPE_WORKERS{4};

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
    std::exception_ptr error;
};

void CoinsStore::WriteWipeBatch(const Wipe& wipe, WipeWorker& worker)
{
    if (!worker.writes.empty()) {
        // Under the flush's two heads, as a run's chunk (BeginWriteOut()).
        base->WriteRun(worker.writes, wipe.best, wipe.old_head, /*final=*/false);
        worker.writes.clear();
        worker.size = 0;
        if (worker.after_first_batch) std::exchange(worker.after_first_batch, nullptr)();
    }
    for (Shard* shard : worker.held) shard->mutex.unlock();
    worker.held.clear();
}

uint64_t CoinsStore::WipeShard(Shard& shard, WipeWorker& worker)
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
    // Where the shard's changes begin in the batch.
    const size_t from{worker.writes.size()};
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
        Transit(entry, Event::Flush);
        if (Pending(entry)) {
            // An open block's change: not the database's until the block is confirmed, and dirty until
            // then. The erase its create went over, or the coin it kept, is the database's now (as in
            // CollectChunk()).
            if (entry.owed) {
                add({it->first, OwedValue(static_cast<int>(entry.coin.nHeight), it->first)});
                Transit(entry, Event::RunWritesOwed, static_cast<int>(entry.coin.nHeight));
                ChargeDelta(entry.height, 1);
            }
            Attach(shard, *it);
            ++it;
        } else {
            // As EraseEntry(), with the coin's usage taken before the coin is moved out.
            const size_t usage{entry.coin.DynamicMemoryUsage()};
            // A marker of a coin the database never held is no erase to write.
            if (entry.dirty && !entry.unwritten) {
                add({it->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(std::move(entry.coin))});
            }
            SubUsage(shard, usage);
            it = shard.map.erase(it);
        }
    }
    // The map hands its entries out in the order of their hashes, and no batch ends during the walk for
    // that reason: what the walk has passed at any point is spread over the shard's whole range of keys,
    // and so is the rest, and two batches with a part each would cover the same range.
    SortWrites(worker.writes, from, worker.sorter);
    shard.map.rehash(0);
    Resized(shard);
    ++shard.settles;
    return added;
}

//! The most shards a flush's worker keeps locked for one batch. Shards with few changes would otherwise
//! all end up in one batch, and ThreadSanitizer's deadlock detector follows no more than 64 mutexes held
//! by a thread. A batch that ends for this reason is whole shards like any other.
static constexpr size_t MAX_BATCH_SHARDS{32};

void CoinsStore::RunWipeWorker(Wipe& wipe, WipeWorker& worker)
{
    Shard* walked{nullptr};
    try {
        for (size_t index{worker.end}; index-- > worker.begin && !wipe.failed.load(std::memory_order_relaxed);) {
            if (worker.held.size() >= MAX_BATCH_SHARDS) WriteWipeBatch(wipe, worker);
            Shard& shard{m_shards[index]};
            // The shard stays locked beyond its walk, among the others the batch took changes from, and
            // they are unlocked together and not in the reverse order: the lock order checks know of the
            // shard for its walk only.
            EnterCritical("shard.mutex", __FILE__, __LINE__, static_cast<ShardMutex::unique_lock::mutex_type*>(&shard.mutex));
            shard.mutex.lock();
            walked = &shard;
            const uint64_t added{WipeShard(shard, worker)};
            walked = nullptr;
            LeaveCritical();
            worker.held.push_back(&shard);
            // A batch ends between two shards when it is full, or when the next shard, taken to be as
            // large as this one, would not fit. A shard larger than a batch is a batch of its own, which
            // the database cuts at its batch size along the keys (CCoinsViewDB::WriteRun()).
            if (worker.size >= wipe.limit || worker.size + added > wipe.limit) WriteWipeBatch(wipe, worker);
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
}

void CoinsStore::Flush(bool /*reallocate_cache*/)
{
    // One pass: a shard's changes are written as its entries are taken out of it, the coins moved rather
    // than copied. A shard stays locked until the batch with the last of its changes is in the database: a
    // reader of the shard (an applier, when the flush is forced on a running node) waits for the write
    // rather than finding a coin in neither place. Every call of WriteRun() names the same two heads, and
    // a last one closes the marker. An open block's own changes stay, dirty as they are; what the block
    // changed is written: a change owed under its create with the shard, a confirmed coin it spent from
    // its record once the shards are through.
    //
    // The batches are ranges of the database's keys that do not overlap (SetBaseWrites()). The shards are
    // taken from the highest down, because the database's marker goes out with the first batch and its
    // head-blocks key sorts above every coin's: that batch then covers the top of the range, not all of
    // it. The marker's other keys are in that first batch and in the last, tiny one that closes it, and in
    // none between (CCoinsViewDB::WriteRun()). With several workers each has a range of neighbouring
    // shards of its own, and all but the one with the highest range start once its first batch, the
    // marker's, is in the database.
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    // See below: the shutdown's flush after one that failed fails as well, and writes nothing.
    if (m_flush_failed) throw std::runtime_error{"the coins are not written: an earlier flush failed"};
    // The restored coins are the caller's (FlushStateToDisk() writes them); a wipe does not.
    Assume(m_restored.empty());
    Assert(!m_run_in_flight);
    m_run_in_flight = true;
    m_dropped_through.store(head_height, std::memory_order_relaxed);
    Wipe wipe{.best = best, .old_head = m_written_head, .limit = std::max<uint64_t>(m_batch_bytes, 1)};
    // A worker for every two batches there may be, so that each has batches to take turns with, and no
    // more than there are cores: a change is 38 bytes of a batch at least. A flush of a few batches, a
    // small cache's, has one.
    const uint64_t cores{std::max(1U, std::thread::hardware_concurrency())};
    const uint64_t wanted{m_base_concurrent ? GetDirtyCount() * uint64_t{38} / wipe.limit / 2 : 1};
    std::vector<WipeWorker> workers(std::clamp<uint64_t>(wanted, 1, std::min<uint64_t>({MAX_WIPE_WORKERS, cores, m_shards.size()})));
    for (size_t i{0}; i < workers.size(); ++i) {
        workers[i].begin = m_shards.size() * i / workers.size();
        workers[i].end = m_shards.size() * (i + 1) / workers.size();
    }
    // noexcept: tasks of the pool write into `wipe` and `workers` until they are waited for below, so nothing may
    // leave this scope early. A failed write is the worker's error (RunWipeWorker()); anything else, a failed
    // allocation, terminates.
    [&]() noexcept {
        // The worker of the highest range runs here, and hands the others to the helpers' pool once its first
        // batch is in. A range is written by whoever claims it first: a task of the pool, or this thread once
        // its own range is done. This thread may hold cs_main, and the pool's workers may be busy with tasks
        // waiting for it, so it never waits for a task that has not started: it takes the ranges no task has
        // claimed, and waits only for those a task is writing. A task that comes up once its range is taken
        // returns at once; what it shares with the flush outlives the call.
        struct Helpers {
            Mutex mutex;
            std::condition_variable cv;
            std::vector<bool> claimed GUARDED_BY(mutex);
            size_t running GUARDED_BY(mutex){0};
        };
        const auto helpers{std::make_shared<Helpers>()};
        WITH_LOCK(helpers->mutex, helpers->claimed.assign(workers.size(), false));
        WipeWorker& first{workers.back()};
        if (workers.size() > 1 && m_flush_helpers) {
            first.after_first_batch = [&] {
                for (size_t i{0}; i + 1 < workers.size(); ++i) {
                    // noexcept: a failed write is the worker's error; anything else terminates rather than
                    // leave the flush waiting for the range.
                    auto task{[this, helpers, i, &wipe, &workers]() noexcept {
                        {
                            LOCK(helpers->mutex);
                            if (helpers->claimed[i]) return;
                            helpers->claimed[i] = true;
                            ++helpers->running;
                        }
                        RunWipeWorker(wipe, workers[i]);
                        LOCK(helpers->mutex);
                        --helpers->running;
                        helpers->cv.notify_all();
                    }};
                    // A pool that takes no task leaves the ranges to this thread.
                    if (!m_flush_helpers->Submit(std::move(task), /*front=*/true)) break;
                }
            };
        }
        RunWipeWorker(wipe, first);
        // The ranges no task has claimed (all of them where the highest range had nothing to write), one after
        // the other, from the highest down.
        for (size_t i{workers.size() - 1}; i-- > 0;) {
            {
                LOCK(helpers->mutex);
                if (helpers->claimed[i]) continue;
                helpers->claimed[i] = true;
            }
            RunWipeWorker(wipe, workers[i]);
        }
        WAIT_LOCK(helpers->mutex, lock);
        helpers->cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(helpers->mutex) { return helpers->running == 0; });
    }();
    for (const WipeWorker& worker : workers) {
        if (!worker.error) continue;
        // The changes of the batch that failed are gone from memory and not in the database, which is
        // left under the marker of a run that did not end: the next start replays from the old head.
        // Nothing may close that marker. The run stays in flight for that reason, so that no write-out
        // run is started, and a later flush throws.
        m_flush_failed = true;
        std::rethrow_exception(worker.error);
    }
    // After the shards: a coin spent since its shard was walked was read from the database.
    std::vector<CoinWrite> from_records;
    size_t record_changes{0};
    try {
        CollectRecords(head_height, /*final=*/true, from_records, record_changes);
        if (!from_records.empty()) base->WriteRun(from_records, best, wipe.old_head, /*final=*/false);
        // Every shard's changes are in the database: consistent with the best block again.
        base->WriteRun({}, best, wipe.old_head, /*final=*/true);
    } catch (const std::runtime_error&) {
        // As a worker's failure above: the marker stays open, and nothing writes again.
        m_flush_failed = true;
        throw;
    }
    m_written_head = best;
    m_written_height = head_height;
    m_fell_below_written = false;
    m_run_in_flight = false;
}

void CoinsStore::Sync()
{
    // As Flush(): after a flush or run that failed nothing is written, so that the database stays under the
    // marker of the failed one and the next start replays from its old head.
    if (m_flush_failed) throw std::runtime_error{"the coins are not written: an earlier flush failed"};
    // The restored coins are the caller's (FlushStateToDisk() writes them); BeginWriteOut() would take them
    // into a run that does not write them.
    Assume(m_restored.empty());
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    auto run{BeginWriteOut(/*bound_height=*/head_height, head_height, /*new_head=*/best, /*final=*/true)};
    Assert(run);
    try {
        StreamWriteOut(*run, /*block_at=*/{}, m_batch_bytes);
    } catch (const std::runtime_error&) {
        AbandonWriteOut();
        throw;
    }
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
    // The spent markers of open blocks, with the height each is filed under.
    std::map<COutPoint, int> pending_markers;
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
            // The entry is in a state an entry may rest in (coins_entry::IsStored() says which and why).
            assert(coins_entry::IsStored(StateOf(entry)));
            // What the machine's height rules keep, as far as an entry shows it. A block's create is filed
            // under its coin's height, or below it under the earlier change it owes.
            if (entry.block_create) assert(entry.owed ? entry.height < static_cast<int>(entry.coin.nHeight) : entry.height == static_cast<int>(entry.coin.nHeight));
            // An open block's spent marker is filed under its block's height, and the block's record has the
            // height the entry goes back under (checked with the records, below).
            if (entry.coin.IsSpent() && Pending(entry)) pending_markers.emplace(outpoint, entry.height);
            if (entry.dirty && (entry.owed || !Pending(entry))) ++confirmed_dirty;
            // Each entry is on the list its state calls for.
            const List list{entry.dirty ? LIST_DIRTY : LIST_CLEAN};
            assert(entry.list == list);
            if (list == LIST_CLEAN) ++clean_listed;
        }
        assert(dirty == shard.dirty_count);
        assert(shard.map.size() == shard.entries);
        assert(shard.map.bucket_count() == shard.buckets);
        total_bytes += NodeBytes() * shard.map.size() + memusage::MallocUsage(sizeof(void*) * shard.map.bucket_count()) + memusage::DynamicUsage(shard.dirty_spans) + usage;
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
    assert(SumTotals(&Totals::bytes) == total_bytes);
    assert(GetCacheSize() == total_entries);
    assert(SumTotals(&Totals::dirty) == total_dirty);
    // A confirmed coin an open block spent is in the block's record, with what the database is owed for it.
    const int confirmed{m_confirmed_height.load(std::memory_order_relaxed)};
    for (const WindowBlock& slot : m_window) {
        if (slot.status.load(std::memory_order_acquire) != BlockStatus::APPLIED) continue;
        LOCK(slot.mutex);
        for (const Undo& item : slot.record) {
            if (!item.spend || item.dead) continue;
            // The height the record keeps for the entry is at or below the block's, where the marker is.
            assert(item.height <= slot.height.load(std::memory_order_relaxed));
            if (const auto marker{pending_markers.find(item.outpoint)}; marker != pending_markers.end() && marker->second == slot.height.load(std::memory_order_relaxed)) {
                pending_markers.erase(marker);
            }
            const bool pending_create{item.created && static_cast<int>(item.coin.nHeight) > confirmed};
            if (pending_create ? item.owed : item.dirty) ++confirmed_dirty;
        }
    }
    // Every open block's marker has the record that says where it goes back.
    assert(pending_markers.empty());
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
    StopFetching(); // as in Confirm(): the workers read the identity
    if (HasBlock()) {
        m_store.UnwindBlock(m_block);
        m_block = CoinsStore::NO_BLOCK;
    }
}

void BlockApplyView::BeginBlock(int height, bool mark_fresh_spends)
{
    Assert(!HasBlock());
    m_block = m_store.BeginBlock(height, mark_fresh_spends);
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
    m_store.ConfirmBlock(block);
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

bool BlockApplyView::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Coin local;
    Coin& spent{moveto ? *moveto : local};
    if (!CoinsViewOverlay::SpendCoin(outpoint, &spent)) return false;
    // What the view keeps of a coin the base had is the spend. It keeps with it which coin that was, for
    // the store to compare at the apply (CoinsStore::Change::known_coin).
    if (const auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
        it->second.coin.nHeight = spent.nHeight;
        it->second.coin.fCoinBase = spent.fCoinBase;
    }
    return true;
}

void BlockApplyView::AnnounceCreates(const CBlock& block)
{
    Assert(HasBlock());
    m_store.AnnounceCreates(m_block, block.vtx);
}

void BlockApplyView::ApplyChanges()
{
    Assert(HasBlock());
    if (m_dirty_count == 0) {
        m_store.CreatesApplied(m_block);
        return;
    }
    std::vector<CoinsStore::Change> changes;
    changes.reserve(m_dirty_count);
    const uint32_t own_height{static_cast<uint32_t>(m_store.BlockHeight(m_block))};
    for (auto& [outpoint, entry] : cacheCoins) {
        if (!entry.IsDirty()) continue;
        // A dirty spent entry is the spend of a coin the base had (a fresh one spent is erased from the view
        // instead); a dirty unspent entry is a create, which overwrote a coin the base had unless fresh.
        // The coin spent is known by its height (SpendCoin()), unless it is one the block made itself over
        // a coin of the base: the store then holds what was there before.
        const bool known_coin{entry.coin.IsSpent() && entry.coin.nHeight != own_height};
        // Only a coinbase may overwrite an unspent coin (the chain's two BIP30 duplicates): a transaction
        // that creates over one repeats an earlier transaction whose inputs are spent, and its block is
        // invalid. Such a create meets the coin it would overwrite as a conflict and changes nothing, so
        // the block's unwind leaves the coin as it was. The rule relies on BIP30 or BIP34 keeping such a
        // block out: with neither (-test=bip30_off on a chain where BIP34 is not active yet) a block that
        // spends an output and makes it again with a repeated transaction is not handled.
        const bool may_overwrite{!entry.IsFresh() && !entry.coin.IsSpent() && entry.coin.IsCoinBase()};
        changes.push_back({.outpoint = outpoint, .coin = std::move(entry.coin), .possible_overwrite = may_overwrite, .must_be_new = m_creates_must_be_new, .known_coin = known_coin});
    }
    // The references the core held into the view are gone with the block's validation; what is applied is
    // read from the store from now on, where this block sees it. A conflict with another applied block is
    // recorded against the loser and settled at confirmation; the core is not told, so that its verdict
    // stays the block's own.
    cacheCoins.clear();
    cachedCoinsUsage = 0;
    m_dirty_count = 0;
    m_store.ApplyBatch(m_block, std::move(changes));
    // The block's outputs are in the store, a deferred transaction's too: a higher block reads them there.
    m_store.CreatesApplied(m_block);
}

void BlockApplyView::Flush(bool /*reallocate_cache*/)
{
    Confirm();
}

std::optional<Coin> BlockApplyView::ReadBase(const COutPoint& outpoint) const
{
    // A window block's read leaves the coin in the tier, clean (SetKeepBaseReads()).
    return m_keep_base_reads ? m_store.LookupFor(m_block, outpoint) : m_store.PeekCoinFor(m_block, outpoint);
}

void BlockApplyView::Reset() noexcept
{
    StopFetching(); // as in Confirm(): the workers read the identity
    if (HasBlock()) {
        // The block was neither confirmed nor unwound: it failed or was abandoned.
        m_store.UnwindBlock(m_block);
        m_block = CoinsStore::NO_BLOCK;
    }
    m_creates_must_be_new = false;
    m_keep_base_reads = false;
    CoinsViewOverlay::Reset();
}
