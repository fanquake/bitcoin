// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINSSTORE_H
#define BITCOIN_COINSSTORE_H

#include <coins.h>
#include <crypto/common.h>
#include <kernel/caches.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>
#include <util/hasher.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * The young tier of the UTXO set: the in-memory layer above the coins
 * database that holds every coin changed since it was last written out and
 * clean copies of old coins that lookups brought in. It is the chainstate's
 * tip view, and it is a CCoinsViewCache to its callers: the same entry
 * semantics (an entry is DIRTY when it differs from the database and FRESH
 * when the database is known not to hold it, so a spent FRESH entry needs no
 * erase) behind the same interface, with the base class's map unused.
 *
 * It differs from CCoinsViewCache in four ways:
 *
 *  - The map is sharded by the leading bytes of the txid, a shard being a
 *    range of the database's keys, with one mutex per shard, held
 *    across the lookup, compare and update of one entry, so that several
 *    threads can mutate distinct entries at once. The shards are many
 *    against the threads (DEFAULT_SHARDS), and what the owner asks of the
 *    whole tier for every block, its size, is kept beside them (Totals).
 *  - Every entry carries the height of its last change. The write-out does
 *    not empty the tier; it takes the entries changed at or below a height
 *    bound (BeginWriteOut), streams them to the database a chunk at a time
 *    under its two-head marker (StreamWriteOut), each chunk becoming clean
 *    as it lands, and ends by recording the head the database then holds
 *    (CommitWriteOut). What stays in memory is the lag: the changes of the
 *    last blocks, among which coins created and spent since the bound are
 *    never written at all.
 *  - Clean entries ("Cached") can be evicted on their own (EvictClean), so
 *    the budget is met before any dirty entry has to be written.
 *  - A change made while connecting a block is made under that block's
 *    identity (BeginBlock, ApplyCreate, ApplySpend) and is complete when it
 *    is made. A created coin is dirty at once, on the list of the block's
 *    height, and reads as not there yet to whoever stands below that height
 *    while the confirmed height is below it too. A spent coin leaves its
 *    entry at once: the entry is erased when the database never held the
 *    coin, and becomes a spent marker under the block's height when the
 *    database is owed the erase; the coin goes to the block's record, where
 *    readers and the appliers of lower blocks find it until the block is
 *    confirmed. Confirming a block is one store of the confirmed height
 *    (ConfirmBlock), after which its creates and markers read as plain
 *    ones: no entry is visited a second time. A block that fails is taken
 *    back from its record (UnwindBlock). Two applied blocks spending one
 *    coin are told apart by height: the lower spend stands, the higher
 *    block gets a conflict and fails at its confirmation.
 *
 * The owner serialises confirmations, unwinds and runs (cs_main today); the
 * shard locks make lookups and applies from other threads safe meanwhile.
 * AccessCoin() returns a reference into a shard, valid only while the
 * owner's lock keeps the entry from being erased.
 */
class CoinsStore : public CCoinsViewCache
{
    struct Entry;
    //! A map node: an outpoint and its entry. Nodes never move, so the lists and a run in flight refer to
    //! entries by node pointer.
    using Node = std::pair<const COutPoint, Entry>;

public:
    //! The identity of a block being connected, from BeginBlock(); 0 is "no block" (a confirmed change).
    using BlockId = uint32_t;
    static constexpr BlockId NO_BLOCK{0};

    //! A write-out run between BeginWriteOut() (or PrepareWriteOut()) and CommitWriteOut().
    struct WriteOutRun {
        //! The changes of a run collected at once (PrepareWriteOut()), in no particular order; a streamed
        //! run (StreamWriteOut()) writes its changes a chunk at a time and leaves this empty.
        std::vector<CoinWrite> writes;
        //! The entries of `writes`, settled at commit unless changed since collection.
        std::vector<Node*> nodes;
        //! The number of changes collected.
        size_t count{0};
        //! The tier's best block when the run was prepared: the marker's new head. Every write is the
        //! outpoint's value in that block's state.
        uint256 new_head;
        //! The block whose state the database held before the run (the settled head of the previous run):
        //! the marker's old head.
        uint256 old_head;
        //! Every change at or below this height is carried.
        int bound_height{-1};
        //! The height of new_head.
        int head_height{-1};
        //! The lowest change height among the dirty entries the run leaves behind, as far as collected.
        int oldest_left{std::numeric_limits<int>::max()};
        //! The height whose state the database holds once the run has landed: that of new_head when no
        //! dirty change remains behind, else one below the oldest change that remains. At least
        //! bound_height, since everything at or below the bound is carried. Known once every shard has
        //! been collected.
        int settled_height{-1};
        //! The block at settled_height on the chain of new_head: the next run's old head. A streamed run
        //! fills it in; after PrepareWriteOut() the caller does, before CommitWriteOut().
        uint256 settled_head;
        //! Whether the run drains the tier: the database is consistent with new_head afterwards and the
        //! marker is replaced by the best block.
        bool final{false};
        size_t bytes{0};
    };

    /**
     * The number of shards unless the caller names one. A thread that finds a shard taken waits for it,
     * and the wait costs far more than the hold it waits out. The chance that an acquisition meets a
     * holder is the share of its time a thread holds a shard, times the threads, over the shards: the
     * shards are many against any number of appliers a machine runs. An empty shard is a few hundred bytes.
     */
    static constexpr size_t DEFAULT_SHARDS{1024};
    explicit CoinsStore(CCoinsView* base_view, size_t shards = DEFAULT_SHARDS, bool deterministic = false);
    ~CoinsStore() override;

    // CCoinsView: the confirmed state, as readers see it.
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    bool HaveCoin(const COutPoint& outpoint) const override EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    uint256 GetBestBlock() const override;
    //! A child cache's dirty entries become confirmed changes of this tier at the current change height.
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;

    // CCoinsViewCache: confirmed changes with no block identity (the disconnect path, snapshot loading, tests).
    void SetBestBlock(const uint256& block_hash) override;
    bool HaveCoinInCache(const COutPoint& outpoint) const override EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    const Coin& AccessCoin(const COutPoint& outpoint) const override EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite) override;
    //! Insert a coin known to be absent from the tier and the base (snapshot loading). Marked dirty, not fresh.
    void EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin) override;
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! Write everything dirty synchronously through the base, as a final run, and drop every entry.
    void Flush(bool reallocate_cache = true) override;
    //! Write everything dirty synchronously through the base, as a final run, and keep the entries clean.
    void Sync();
    /**
     * How the base takes its writes: the size of a batch (the database's batch size, the size its writes
     * are measured to be good at), and whether several threads may call its WriteRun() at once (the
     * database's may). Flush() and Sync() write batches of up to that size, filled across neighbouring
     * shards. A flush of more than a few batches to a base that allows it runs on several threads, a
     * range of the shards each, collecting and serializing its next batch while the database is busy
     * with another's.
     */
    void SetBaseWrites(uint64_t batch_bytes, bool concurrent)
    {
        m_batch_bytes = batch_bytes;
        m_base_concurrent = concurrent;
    }
    void Uncache(const COutPoint& outpoint) override;
    unsigned int GetCacheSize() const override;
    size_t GetDirtyCount() const noexcept override;
    size_t DynamicMemoryUsage() const override;
    void SanityCheck() const override EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! Not meaningful for the tier.
    void Reset() noexcept override;

    // The tier's own interface.
    //! Set the block and the height tagged onto the changes that follow.
    void SetBestBlock(const uint256& block_hash, int height);
    //! The height tagged onto the changes that follow. Set to the fork height before disconnecting blocks, so
    //! that the inverse changes are the oldest and the next run carries them all.
    void SetChangeHeight(int height) { m_change_height = height; }
    int GetChangeHeight() const { return m_change_height; }
    //! The height of the tier's best block, for the paths that move it by hash alone (a disconnect's
    //! flush); SetBestBlock(hash, height) sets it too. Distinct from the change height, which a reorg
    //! holds at the fork while the best block moves.
    void SetTierHeight(int height)
    {
        m_tier_height = height;
        m_confirmed_height.store(height, std::memory_order_release);
    }
    int TierHeight() const { return m_tier_height; }

    // Connecting a block: changes with the block's identity, invisible to readers until confirmation.
    //! Open a block of `height` in the window table. Serialised by the owner. The height is above the
    //! confirmed height (that of the block confirmed last, or given with the best block), which is what
    //! tells an open block's changes from confirmed ones; only a disconnect lowers the confirmed height.
    BlockId BeginBlock(int height) EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! The coin as block `block` sees it: created by a confirmed or applied block (or long ago), and not spent
    //! by a confirmed or applied block. Brings old coins in from the base.
    std::optional<Coin> LookupFor(BlockId block, const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! The same without bringing anything in (the prefetch workers).
    std::optional<Coin> PeekCoinFor(BlockId block, const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! Block `block` creates `outpoint`. A live coin under it is a BIP30 overwrite, allowed only with
    //! `possible_overwrite`, as AddCoin().
    void ApplyCreate(BlockId block, const COutPoint& outpoint, Coin&& coin, bool possible_overwrite) EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    /**
     * Block `block` spends `outpoint`, which it saw (LookupFor) when it checked its inputs. Another applied
     * block may have spent it since: the lower height stands. If that block is lower, this block becomes
     * the offender (its conflict count rises, the spend is not recorded); if higher, this block displaces
     * it and the other block gets the conflict. A spend the block did not see is not recorded either and
     * counts as a conflict; that includes a coin the database never held and a higher block spent first,
     * which left the tier with that spend: the owner unwinds the higher blocks and connects this one again.
     * The coin is copied to `copy` when given (for the undo data). Returns whether the spend was recorded.
     */
    bool ApplySpend(BlockId block, const COutPoint& outpoint, Coin* copy) EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! One of a block's changes, for ApplyBatch(): an unspent coin is the outpoint's creation, a spent one
    //! its spend.
    struct Change {
        COutPoint outpoint;
        Coin coin;
        bool possible_overwrite{false};
    };
    /**
     * Block `block` makes `changes`, as ApplyCreate() and ApplySpend() would one by one, but grouped by
     * shard so that each shard's lock is taken once for the block instead of once per change. The appliers
     * run on many threads at once, and a lock taken per change moves the shard's lines between their cores
     * on every change; taken per block, it moves once per shard.
     */
    void ApplyBatch(BlockId block, std::vector<Change>&& changes) EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! The coins among `outpoints` that block `viewer` sees and the tier holds, read with one hold of each
    //! shard's lock; what is missing, spent or invisible is left out (PeekCoinFor() one by one is the fallback).
    std::vector<std::pair<COutPoint, Coin>> FetchFor(BlockId viewer, std::vector<COutPoint>&& outpoints) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! The conflicts recorded against the block so far; nonzero means it must not confirm.
    uint32_t ConflictCount(BlockId block) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! The block's height, as given to BeginBlock().
    int BlockHeight(BlockId block) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    /**
     * The block becomes part of the confirmed state: the confirmed height becomes its height, which is what
     * its creates and its spent markers are told from plain ones by, and the identity is freed. No entry
     * is visited. With a pool, the block's record is destroyed there rather than here.
     */
    void ConfirmBlock(BlockId block, ThreadPool* pool = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! The block's changes are taken back from its record, the last first: its creates disappear, the coins
    //! it spent are put back as they were. The identity is freed. Blocks are unwound from the highest down
    //! (a coin a lower block created and a higher one spent goes with the higher block's unwind), and not
    //! while a run collects (StreamWriteOut(), PrepareWriteOut(), Flush()).
    void UnwindBlock(BlockId block) EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! Blocks opened and neither confirmed nor unwound.
    size_t OpenBlocks() const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);

    //! Bytes of dirty entries whose change height is at most `height`.
    size_t DirtyBytesAtOrBelow(int height) const;
    //! The lowest height bound whose write-out frees at least `bytes` of dirty entries, or the tip height if
    //! all dirty entries together do not.
    int BoundForBytes(size_t bytes, int tip_height) const;
    //! The lowest change height among dirty entries, or nullopt if none.
    std::optional<int> OldestDirtyHeight() const;

    //! Drop clean entries that no open block refers to until the tier's usage is at most `target_bytes`,
    //! oldest first in each shard; an entry read since it was last passed over gets a second chance.
    size_t EvictClean(size_t target_bytes);

    /**
     * Open a run that carries every dirty entry changed at or below `bound_height`, under the
     * marker (`new_head`, the settled head of the previous run); `head_height` is new_head's height.
     * `final` carries everything regardless of the bound. A run with nothing to carry still
     * moves the settled head when the bound is above it; nullptr if there is nothing to carry and no
     * head to move. An entry an open block holds is dirty only for a confirmed change and is carried at
     * its confirmed value; the block's own changes are no part of new_head's state.
     *
     * What the marker promises: before the run the database holds the state at old_head; each write sets
     * an outpoint to its value at new_head; so a crash anywhere inside the run is repaired by undoing
     * old_head down to its fork with new_head and re-applying up to new_head (ReplayBlocks), which
     * rewrites every outpoint the two states can differ in. For the promise to hold at the next run, the
     * settled head after this one must be a block whose state the database then holds, which is
     * `settled_height` on new_head's chain: the bound names the oldest change carried, not that state
     * (a reorg's inverse changes are tagged with the fork height and move the state to the tier's head).
     */
    std::unique_ptr<WriteOutRun> BeginWriteOut(int bound_height, int head_height, const uint256& new_head, bool final);
    /**
     * Write the run through the base a chunk at a time, a chunk being one database batch of about
     * `batch_bytes` (the database's batch size): collect a chunk of dirty entries from the shards'
     * dirty lists, each shard under its lock and the chunk filling across shards, write
     * it with no shard locked, then settle the chunk's entries shard by shard through the node pointers
     * kept (an entry changed since stays dirty and is collected again; a spent one is erased). Memory
     * beyond the tier is one chunk's coins, whatever the size of the run, and a run smaller than a chunk is one
     * write. A collected entry is no longer FRESH: its value is on its way to the database, and a spend
     * meanwhile must produce an erase. At most one chunk is outstanding, and a collected entry is never
     * erased before its chunk settles, which is what keeps the pointers good.
     *
     * The last write closes the marker: a final run replaces it by the best block; any other rewrites
     * it to (new_head, settled head), the pair the next run starts from, so that the marker on disk
     * never names an old head below what the tier counts as written. `block_at` names the block at a
     * height on new_head's chain (not needed for a final run).
     *
     * Takes shard locks only and touches nothing else of the tier, so it may run on another thread than
     * the owner's while the owner makes no confirmed change. Throws what the base's WriteRun() throws;
     * the run is then abandoned (AbandonWriteOut()).
     */
    void StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes = DEFAULT_DB_CACHE_BATCH);
    //! BeginWriteOut() with the whole run collected at once into `writes`, for a caller that writes it
    //! itself; nullptr also if nothing collected and the run is not final.
    std::unique_ptr<WriteOutRun> PrepareWriteOut(int bound_height, int head_height, const uint256& new_head, bool final);
    //! The run has landed: entries collected at once and unchanged since become clean (spent ones are
    //! erased), and the run's settled head is the database's state from now on.
    void CommitWriteOut(const WriteOutRun& run);
    //! The run did not land (the node is going down): entries stay dirty, the tier can be used again.
    void AbandonWriteOut();
    //! The settled head: the block whose state the database holds, apart from the run in flight (null
    //! before any run).
    const uint256& WrittenHead() const { return m_written_head; }
    int WrittenHeight() const { return m_written_height; }
    //! At start-up, after the database was found or made consistent with `block_hash`: nothing is dirty.
    void InitWrittenHead(const uint256& block_hash, int height)
    {
        m_written_head = block_hash;
        m_written_height = height;
        SetTierHeight(height);
    }
    //! Whether a run is between BeginWriteOut() (or PrepareWriteOut()) and CommitWriteOut().
    bool RunInFlight() const { return m_run_in_flight; }

    //! What a thread is doing with the store when it takes a shard.
    enum class Site : uint8_t { OTHER, LOOKUP, FETCH, APPLY, UNWIND, WRITE_OUT };
    static constexpr size_t NUM_SITES{6};
    static const char* SiteName(Site site);
    //! The acquisitions of a shard that found it held, and the time they waited, by what the waiter was
    //! doing and what the holder was.
    struct LockWait {
        Site waiter;
        Site holder;
        uint64_t count;
        //! Those of them that the retries did not get through, and that went to sleep.
        uint64_t slept;
        std::chrono::nanoseconds time;
    };
    //! For the bench: every pair of sites that waited at all, over all stores of the process. Nothing
    //! in a build with the lock order checks (see ShardMutex).
    static std::vector<LockWait> LockWaits();

private:
    //! UNWOUND: being taken back (UnwindBlock()); the record is still the block's until that is through.
    enum class BlockStatus : uint8_t { FREE, APPLIED, UNWOUND };
    /**
     * One change of an open block, as the block's unwind needs it, and as those need it who are to see
     * the coin the block spent: readers, and the appliers of lower blocks.
     */
    struct Undo {
        //! A spend: the coin. A create: empty.
        Coin coin;
        COutPoint outpoint;
        //! A spend: the entry's change height before it.
        int32_t height{0};
        bool spend : 1 {false};
        //! A spend: the entry's flags before it (Entry), `created` being its block_create. Dirty and not
        //! fresh, with no entry, once a run wrote the coin from here.
        bool dirty : 1 {false};
        bool fresh : 1 {false};
        bool created : 1 {false};
        bool owed : 1 {false};
        //! No longer the block's: a lower block took the spend over, or the unwind is past it.
        bool dead : 1 {false};
    };
    /**
     * A block identity's slot. Status and height are read by lookups from every worker at once, so they
     * are atomics read without a lock: the height is written before the status that makes the slot
     * visible (release), and read after it (acquire). The slot's own mutex guards the conflict count and
     * the record, which the block's worker writes and which a reader, a lower block's applier or a run
     * reads now and then; it is taken with a shard held, never the other way round, and a second slot's
     * only by the lower block of the two. Slots never move: an identity is an index.
     */
    struct WindowBlock {
        std::atomic<BlockStatus> status{BlockStatus::FREE};
        std::atomic<int> height{0};
        /**
         * What the block's changes add to the tier's dirty entries over what they add to the confirmed
         * state's (nothing, until the block is confirmed): GetDirtyCount() takes it off again. Exact
         * when blocks are unwound from the highest down.
         */
        mutable std::atomic<int64_t> dirty_delta{0};
        mutable Mutex mutex;
        uint32_t conflicts GUARDED_BY(mutex){0};
        //! The block's changes in the order they were made.
        std::vector<Undo> record GUARDED_BY(mutex);
    };
    //! Enough slots for the widest window and the tip view, with room to spare.
    static constexpr size_t MAX_WINDOW_SLOTS{64};
    //! Which of its shard's lists an entry is on.
    enum List : uint8_t { LIST_NONE, LIST_DIRTY, LIST_CLEAN };
    /**
     * An entry is on one list of its shard, by its state: a dirty entry on the list of its change height,
     * where the write-out finds it without a scan of the map; a clean entry on the clean list, in the
     * order it became clean or was brought in, where eviction finds it. The flags share two bytes, which
     * pays for the links: the entry is no larger than it was without them.
     *
     * Three flags are left by changes made under a block identity, and mean something only while the
     * confirmed height is below the height they refer to: the entry is then pending, an open block's,
     * which the write-out passes over. A clean entry has none of them.
     */
    struct Entry {
        Coin coin;
        //! The height of the last change, the write-out's age.
        int32_t height{0};
        bool dirty : 1 {false};
        bool fresh : 1 {false};
        //! A run collected the entry's value and has not settled it; any change clears the bit (Touch),
        //! and the entry then stays dirty when the run's chunk settles.
        bool collected : 1 {false};
        //! A lookup found the entry since eviction last passed over it.
        bool referenced : 1 {false};
        //! An unspent coin a block created, at the coin's height: pending while the confirmed height is below it.
        bool block_create : 1 {false};
        //! A spent entry a block's spend left for the erase the database is owed, the change height
        //! being the block's: pending while the confirmed height is below it, the coin in the block's record.
        bool block_spend : 1 {false};
        //! With block_create: the create went over an erase the database is owed still, whose change
        //! height the entry keeps. A run that passes writes the erase and moves the entry to the coin's height.
        bool owed : 1 {false};
        uint8_t list : 2 {LIST_NONE};
        Node* prev{nullptr};
        Node* next{nullptr};
    };
    using Map = std::unordered_map<COutPoint, Entry, SaltedCoinsCacheHasher>;
    //! The dirty entries of one span of change heights in a shard.
    struct Bucket {
        //! What writing them out frees: DirtyBytes() of each.
        int64_t bytes{0};
        Node* head{nullptr};
    };
    /**
     * What a shard's mutex is built on. A shard is held for a few entries and free again well within the
     * time it takes to put a waiting thread to sleep and to wake it up, so a waiter that sleeps at once
     * pays that time for a hold it would have outlasted, at every collision. It retries for about as long
     * as the sleep and the wake-up would take before it blocks. (A write-out holds a shard for longer; its
     * waiters block once the retries are spent.)
     *
     * An acquisition that found the mutex held is counted and timed by what the waiter and the holder
     * were doing (LockWaits()): the holder leaves its site in the mutex when it takes it.
     */
    class ShardLock
    {
    public:
        void lock();
        bool try_lock();
        void unlock() { m_mutex.unlock(); }

    private:
        std::mutex m_mutex;
        //! The Site of the thread that holds the mutex, read by a waiter without it.
        std::atomic<uint8_t> m_holder{0};
    };
#ifdef DEBUG_LOCKORDER
    //! The lock order checks know the standard mutexes only: such a build has neither the retries nor the counts.
    using ShardMutex = Mutex;
#else
    using ShardMutex = AnnotatedMixin<ShardLock>;
#endif
    //! On cache lines of its own: a shard is written by whichever thread holds it, and its neighbours by others.
    struct alignas(64) Shard {
        mutable ShardMutex mutex;
        Map map GUARDED_BY(mutex);
        //! The shard's share of the tier's totals (see Totals): the memory of its coins, its dirty entries,
        //! and its map's size and bucket count as last counted (Resized()).
        size_t coins_usage GUARDED_BY(mutex){0};
        size_t dirty_count GUARDED_BY(mutex){0};
        size_t entries GUARDED_BY(mutex){0};
        size_t buckets GUARDED_BY(mutex){0};
        //! The dirty entries by span of change heights (SpanOf()), empty when the shard has none: the
        //! write-out walks the lists up to its bound, and the byte sums find the span a bound lies in.
        std::vector<Bucket> dirty_spans GUARDED_BY(mutex);
        //! The clean list: entries join at the young end and are evicted from the old end.
        Node* clean_young GUARDED_BY(mutex){nullptr};
        Node* clean_old GUARDED_BY(mutex){nullptr};
        //! Counts the times a write-out settled entries of this shard, or a flush emptied it: an unlocked
        //! read of the base that started before the count moved may predate what was written.
        uint64_t settles GUARDED_BY(mutex){0};
    };
    /**
     * The tier's totals: its memory usage, its entries and its dirty entries. The owner asks for them
     * several times for every block it publishes (the cache size state, the flush decision, the tip's log
     * line), and every change of an entry moves them. They are kept as sums of deltas in stripes, a
     * thread adding to the stripe it was dealt and each stripe on a cache line of its own, so that a
     * change costs its thread a line it has to itself, and a query reads the stripes and neither locks
     * nor reads a shard. One stripe means nothing (what one thread adds another takes away); the sum is
     * the total, exact when no change is under way.
     */
    struct alignas(64) Totals {
        //! As DynamicMemoryUsage() reports it: the maps' nodes and bucket arrays, the shards' arrays of
        //! dirty lists, and the coins' own memory.
        std::atomic<int64_t> bytes{0};
        std::atomic<int64_t> entries{0};
        std::atomic<int64_t> dirty{0};
    };
    static constexpr size_t TOTALS_STRIPES{64};
    //! The calling thread's stripe.
    Totals& MyTotals() const;
    size_t SumTotals(std::atomic<int64_t> Totals::* field) const;
    //! What an entry's map node costs, as DynamicMemoryUsage() counts it.
    static int64_t NodeBytes();
    /**
     * How many change heights share a list of dirty entries, as a power of two. While nothing is written
     * out the dirty entries are the coins of every height since the last write, a few to a shard for each:
     * a list for every height is then an allocation of its own for nearly every dirty coin, and finding it
     * a walk through a shard's hundred thousand lists for every coin a block creates or spends. An entry
     * keeps its height, and whoever needs heights apart inside a span reads them there.
     */
    static constexpr int DIRTY_SPAN_BITS{10};
    static size_t SpanOf(int height);
    //! The list for dirty entries of this change height; the shard's array grows to hold it.
    Bucket& DirtyBucket(Shard& shard, int height) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Give the shard's array of dirty lists back: it has no dirty entry left.
    void FreeDirtySpans(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! The memory of a shard's coins grows or shrinks.
    void AddUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SubUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    Shard& ShardFor(const COutPoint& outpoint) const { return m_shards[ShardIndex(outpoint)]; }
    //! What a dirty entry counts for in the tier's usage, as DynamicMemoryUsage() measures it.
    static int64_t DirtyBytes(const Entry& entry);
    //! Take the entry off the list it is on. Before any change of its coin or height, which its place
    //! and its bucket's byte sum depend on.
    void Detach(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Put a detached entry on the list its state calls for.
    void Attach(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! A lookup found the entry.
    static void Reference(Entry& entry) { if (!entry.referenced) entry.referenced = true; }
    //! The entry changed: a run that collected it must not settle it.
    static void Touch(Entry& entry) { entry.collected = false; }
    //! Collect the shard's dirty entries at or below the run's bound into `writes` and `nodes` until
    //! `size`, about what the collected changes come to in a database batch, reaches `limit`; note the
    //! oldest change height left behind. An open block's entries are passed over, except for the erase
    //! one of its creates went over (Entry::owed), which is written.
    void CollectChunk(Shard& shard, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<Node*>& nodes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    /**
     * The confirmed coins a run owes the database that are in no entry any more: an open block spent
     * them, and they are in its record. Those changed at or below `bound_height` (all of them when
     * `final`) are added to `writes`, and their blocks' spends become what a spend of a written coin is,
     * a spent marker; the lowest change height left behind is returned (the highest int when none is).
     */
    int CollectRecords(int bound_height, bool final, std::vector<CoinWrite>& writes, size_t& count);
    //! A collected entry's value is in the database: clean (erased when spent), unless changed since.
    void SettleNode(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    bool SpendEntry(Shard& shard, Map::iterator it, Coin* moveto) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex, !m_window_mutex);
    //! The entry's coin as a copy, bringing it in from the base on a miss; nullopt if absent or spent.
    std::optional<Coin> Lookup(const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! What an entry is to a viewer.
    enum class Sight : uint8_t {
        //! No coin: spent, or created above where the viewer stands.
        NOTHING,
        //! The entry's coin.
        COIN,
        //! A coin a block spent that does not bind the viewer (an open block for a reader, a higher one
        //! for an applier): in that block's record, under the entry's change height.
        RECORD,
    };
    //! For a reader (`viewer_height` -1) or the applier of the block at `viewer_height`: an applier sees
    //! the creates of lower open blocks and not the coins lower open blocks (or its own) spent; a higher
    //! block's create is not there yet for it, a higher block's spend does not bind it (it displaces it).
    Sight See(const Entry& entry, int viewer_height) const;
    //! Whether the entry is an open block's: a create or a spent marker above the confirmed height.
    bool Pending(const Entry& entry) const;
    //! -1 for a reader (NO_BLOCK), the block's height for an applier.
    int ViewerHeight(BlockId viewer) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex) { return viewer == NO_BLOCK ? -1 : BlockHeight(viewer); }
    //! The slot that holds a block of `height`, applied or being unwound; NO_BLOCK if none does.
    BlockId SlotAt(int height) const;
    //! Add to the dirty delta of the block applied at `height`, if there is one.
    void ChargeDelta(int height, int64_t delta) const;
    //! What a viewer has of an entry it found, with the shard held: a copy of the coin, from the entry or
    //! from the record of the block that spent it.
    std::optional<Coin> SeenCoin(Entry& entry, const COutPoint& outpoint, int viewer_height) const;
    //! The coin the block at `spender_height` spent, as its record holds it, if the viewer is to see it.
    std::optional<Coin> RecordCoin(int spender_height, const COutPoint& outpoint, int viewer_height) const;
    //! For a reader that found the outpoint neither in the tier nor in the base, with the shard held: a
    //! confirmed coin the database never held and an open block spent is in that block's record alone.
    std::optional<Coin> RecordScan(const COutPoint& outpoint) const;
    //! Append `undo` to the block's record and add `delta` to its dirty delta, emptying both. With the
    //! shard of the changes still held: who finds their traces in the shard finds the record complete.
    static void Record(WindowBlock& mine, std::vector<Undo>& undo, int64_t& delta);
    //! A free slot claimed for a block at `height`, or NO_BLOCK when every slot is taken.
    BlockId ClaimSlot(int height) EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    /**
     * Visit the shards of a block's items, `shard_of[i]` being item i's: `work(shard, items)` runs once
     * for every shard that has any, with its lock held and the indices of its items in their order. The
     * walk starts at a shard of the block's own, so that the appliers of different blocks do not go round
     * the shards one behind another, and it passes over a shard another thread holds and comes back for
     * those at the end: there is other work to do meanwhile and no order between shards to keep.
     */
    template <typename Work>
    void VisitShards(BlockId block, const std::vector<uint32_t>& shard_of, Work work) const EXCLUSIVE_LOCKS_REQUIRED(!m_window_mutex);
    //! Erase an entry, keeping the shard's lists and counters right.
    void EraseEntry(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! The same for an entry reached over a list. Erased by its key, which is never more work than a
    //! lookup for an iterator and the erase by it: libstdc++ stores no hash in a node when the hasher is
    //! noexcept, as this one is, so there the two would each hash the key and walk the bucket.
    void EraseEntry(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Erase an entry that is off its list and whose coin's usage is taken off the books already.
    void EraseDetached(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    struct Wipe;
    struct WipeWorker;
    //! A flush's worker: takes its shards from the highest down, and writes what it took out of them.
    void RunWipeWorker(Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    //! Take the shard's entries out of it, its changes into the worker's batch; a full batch is written on
    //! the way. Returns what the shard's changes come to as a database batch.
    uint64_t WipeShard(Shard& shard, const Wipe& wipe, WipeWorker& worker) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Write the worker's batch and unlock the shards it emptied: their changes are in the database.
    void WriteWipeBatch(const Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    //! The map's size or bucket count may have changed: count the difference into the totals.
    void Resized(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! ApplyCreate() for the block at `height` with the shard held. The change goes to `undo` and what it
    //! adds to the dirty entries to `delta`, for the caller to hand to Record() before it lets go of the
    //! shard. False: not made, a conflict for the block (the outpoint is another open block's to settle).
    bool CreateLocked(Shard& shard, int height, const COutPoint& outpoint, Coin&& coin, bool possible_overwrite, std::vector<Undo>& undo, int64_t& delta) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex, !m_window_mutex);
    enum class SpendResult { DONE, CONFLICT, MISS };
    //! ApplySpend() for the block at `height` with the shard held, `undo` and `delta` as for
    //! CreateLocked(). MISS: the entry is not in the tier and `from_base` was not given; the caller reads
    //! the base with the shard unlocked and comes back with the coin. The caller counts a CONFLICT.
    SpendResult SpendLocked(Shard& shard, int height, const COutPoint& outpoint, Coin* copy, const Coin* from_base, std::vector<Undo>& undo, int64_t& delta) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex, !m_window_mutex);
    //! One item of the record of the block at `height`, which is being unwound, taken back with the shard held.
    void UnwindItem(Shard& shard, int height, Undo&& item) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex, !m_window_mutex);
    /**
     * The shard of an outpoint, by the leading bytes of its txid as the database's key begins with them:
     * the shards are consecutive ranges of the database's keys in the order of their index, so that what
     * a write-out takes from neighbouring shards is a batch that covers one range of keys and no other
     * batch's. Txids are uniform, and so are the shards' loads. A transaction's outputs share a shard.
     * Whoever makes a transaction can pick its shard by grinding, which buys contention on that shard
     * and nothing else: an entry's place in the shard's map is by the salted hash.
     */
    size_t ShardIndex(const COutPoint& outpoint) const { return (uint64_t{ReadBE32(outpoint.hash.ToUint256().data())} * m_shards.size()) >> 32; }

    /**
     * The change height an unspent coin arriving outside a block identity is tagged with. A coin
     * restored by a disconnect (the change height is then the fork height) that was created above the
     * settled height is not in the database and must not reach it before its creating block does, so it
     * keeps its creation height; any other coin takes the change height.
     */
    int TagFor(const Coin& coin) const
    {
        const int settled{m_run_in_flight ? m_inflight_settled_height : m_written_height};
        return coin.nHeight > settled && coin.nHeight > m_change_height ? coin.nHeight : m_change_height;
    }

    mutable std::vector<Shard> m_shards;
    mutable std::array<Totals, TOTALS_STRIPES> m_totals;
    mutable uint256 m_tier_block_hash;
    int m_change_height{0};
    int m_tier_height{-1};
    uint256 m_written_head;
    int m_written_height{-1};
    bool m_run_in_flight{false};
    //! The settled height of the run in flight: what the database holds once it lands.
    int m_inflight_settled_height{-1};

    //! The window table, indexed by BlockId; slot 0 is NO_BLOCK. The mutex serialises slot claims only
    //! (BeginBlock); a slot's status and height are read lock-free, the rest under the slot's own mutex.
    mutable Mutex m_window_mutex;
    std::array<WindowBlock, MAX_WINDOW_SLOTS> m_window;
    //! The height of the block confirmed last (or given with the best block): a block's create or spent
    //! marker above it is an open block's. Read by every lookup that meets one, without a lock.
    std::atomic<int> m_confirmed_height{-1};
    //! The slots not free, for the readers' check that there is no record to look into.
    std::atomic<int> m_open_slots{0};
    //! See SetBaseWrites().
    uint64_t m_batch_bytes{DEFAULT_DB_CACHE_BATCH};
    bool m_base_concurrent{false};
};

/**
 * The view a block is connected into: a CoinsViewOverlay (prefetch and the block's own changes for the
 * core's lookups and references) whose changes also go straight into the store under the block's
 * identity, so that nothing has to be moved at publication and other appliers can see them. Confirming
 * the block is one store of the confirmed height in the store, unwinding it takes its changes back from
 * the store's record; the view's reset unwinds a block that was neither.
 */
class BlockApplyView final : public CoinsViewOverlay
{
public:
    BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic = false);
    ~BlockApplyView() override;

    //! Open `height`'s block in the store; the changes that follow carry its identity.
    void BeginBlock(int height);
    CoinsStore::BlockId Block() const { return m_block; }
    bool HasBlock() const { return m_block != CoinsStore::NO_BLOCK; }
    //! The block becomes confirmed state; see CoinsStore::ConfirmBlock(). The view is reset.
    void Confirm();
    //! The block's changes are taken back; see CoinsStore::UnwindBlock(). The view is reset.
    void Unwind();
    uint32_t Conflicts() const;

    //! Bring the block's inputs that the tier holds into the view, one hold of each shard's lock, so that
    //! the block's validation reads the store once per shard instead of once per input.
    void Prefetch(const CBlock& block);
    //! Apply the view's changes so far to the store under the block's identity, grouped by shard, and drop
    //! them from the view; the next lookups read the store, where they are visible to this block. Called by
    //! the block's worker once validated (and again after a continuation), so that the work is off the
    //! publisher; Confirm() applies what is left.
    void ApplyChanges();
    //! Not used: Confirm() applies the changes and makes them confirmed state.
    void Flush(bool reallocate_cache = true) override;

protected:
    std::optional<Coin> ReadBase(const COutPoint& outpoint) const override;
    void Reset() noexcept override;

private:
    CoinsStore& m_store;
    CoinsStore::BlockId m_block{CoinsStore::NO_BLOCK};
};

#endif // BITCOIN_COINSSTORE_H
