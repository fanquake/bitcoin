// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINSSTORE_H
#define BITCOIN_COINSSTORE_H

#include <coins.h>
#include <coins_entry_machine.h>
#include <crypto/common.h>
#include <kernel/caches.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>
#include <util/hasher.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <tuple>
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
 *  - The map is sharded by the leading bytes of the txid, a shard being a
 *    range of the database's keys, with one mutex per shard, held across the
 *    lookup, compare and update of one entry, so that several threads can
 *    mutate distinct entries at once.
 *  - Every entry carries the height of its last change. A write-out run
 *    takes the entries changed at or below a height bound (BeginWriteOut),
 *    streams them to the database a chunk at a time under its two-head
 *    marker (StreamWriteOut), each chunk becoming clean as it lands, and
 *    ends by recording the head the database then holds (CommitWriteOut).
 *    What stays in memory is the lag: the changes of the last blocks, among
 *    which coins created and spent since the bound are never written at all.
 *  - Clean entries can be evicted on their own (EvictClean), so the budget
 *    is met before any dirty entry has to be written.
 *  - A change made while connecting a block is made under that block's
 *    identity (BeginBlock, ApplyBatch) and is complete when it is made. A
 *    created coin is dirty at once, on the list of the block's height (one
 *    made over a change the database is still owed stays on the list of
 *    that change), and reads as not there yet to whoever stands below that
 *    height while the confirmed height is below it too. A spent coin leaves
 *    its entry at once: the entry is erased when the database never held
 *    the coin, and becomes a spent marker under the block's height when the
 *    database is owed the erase, or when a lower open block may create the
 *    outpoint again (BIP30, see BeginBlock); the coin goes to the block's
 *    record, where readers find it until the block is confirmed, and from
 *    where a lower block that spends it too takes it through the marker.
 *    Two applied blocks spending one coin are told apart by height: the
 *    lower spend stands and the higher block gets a conflict (ApplySpend).
 *    Confirming a block is one store of the confirmed height (ConfirmBlock),
 *    after which its creates and markers read as plain ones; a block that
 *    fails is taken back from its record (UnwindBlock).
 *
 * The owner serialises confirmations, unwinds and runs (under cs_main); the
 * shard locks make lookups and applies from other threads safe meanwhile.
 * AccessCoin() returns a reference to the calling thread's copy of the coin,
 * valid until the thread's next call: no lock of the caller keeps an applier
 * from spending the entry.
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
    //! Enough slots for the widest window and the tip view, with room to spare. Open blocks are consecutive
    //! blocks of one chain, so no two of them are this many heights apart.
    static constexpr size_t MAX_WINDOW_SLOTS{128};

    //! A write-out run between BeginWriteOut() and CommitWriteOut().
    struct WriteOutRun {
        //! Coins a disconnect restored (KeepWritten()), handed to this run: written before anything else of
        //! it and before its marker, as plain coins (CCoinsViewDB::WriteCoins()).
        std::vector<CoinWrite> restored;
        //! The number of changes collected.
        size_t count{0};
        //! The number of database batches a streamed run wrote them in.
        size_t chunks{0};
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
        //! The block at settled_height on the chain of new_head: the next run's old head. StreamWriteOut()
        //! fills it in.
        uint256 settled_head;
        //! Whether the run drains the tier: the database is consistent with new_head afterwards and the
        //! marker is replaced by the best block.
        bool final{false};
        size_t bytes{0};
    };

    //! The number of shards unless the caller names one: many against any number of appliers a machine
    //! runs, since a thread that finds a shard taken waits far longer than the hold it waits out. An
    //! empty shard is a few hundred bytes.
    static constexpr size_t DEFAULT_SHARDS{256};
    //! `deterministic` is the cache base's (CCoinsViewCache), whose own map the store does not use; the shards'
    //! key hash keeps its random salt (KeyHash()), which places entries and decides no outcome.
    explicit CoinsStore(CCoinsView* base_view, size_t shards = DEFAULT_SHARDS, bool deterministic = false);
    ~CoinsStore() override;

    // CCoinsView: the confirmed state, as readers see it.
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    //! A child cache's dirty entries become confirmed changes of this tier at the current change height.
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;

    // CCoinsViewCache: confirmed changes with no block identity (the disconnect path, snapshot loading, tests).
    void SetBestBlock(const uint256& block_hash) override;
    bool HaveCoinInCache(const COutPoint& outpoint) const override;
    const Coin& AccessCoin(const COutPoint& outpoint) const override;
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite) override;
    //! Insert a coin known to be absent from the tier and the base (snapshot loading). Marked dirty, not fresh.
    void EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin) override;
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    //! Write everything dirty synchronously through the base, as a final run, and drop every entry but the
    //! open blocks' own, which stay as they are. The coins disconnects restored are not among what it
    //! writes: the caller writes Restored() first.
    void Flush(bool reallocate_cache = true) override;
    //! Write everything dirty synchronously through the base, as a final run, and keep the entries clean.
    //! As Flush(), the caller writes Restored() first.
    void Sync();
    /**
     * How the base takes its writes: the size of a batch (the database's batch size), and whether several
     * threads may call its WriteRun() at once (the database's may). Flush() and Sync() write batches of up
     * to that size, each the changes of neighbouring shards in the order of their keys: a range of the
     * database's keys that no other batch of the flush reaches into (ShardIndex()). A flush of more than
     * a few batches to a base that allows it has up to four workers, a range of the shards each,
     * collecting and serializing its next batch while the database is busy with another's: the calling
     * thread and tasks of the pool given to SetFlushHelpers().
     */
    void SetBaseWrites(uint64_t batch_bytes, bool concurrent)
    {
        m_batch_bytes = batch_bytes;
        m_base_concurrent = concurrent;
    }
    //! The pool a flush's further workers run on, as tasks (the node's connect pool). Without one, the
    //! calling thread writes every range, one after the other.
    void SetFlushHelpers(std::shared_ptr<ThreadPool> pool) { m_flush_helpers = std::move(pool); }
    void Uncache(const COutPoint& outpoint) override;
    unsigned int GetCacheSize() const override;
    size_t GetDirtyCount() const noexcept override;
    size_t DynamicMemoryUsage() const override;
    //! The regions the nodes of every store are carved from, which are never given back: beyond the live
    //! nodes, which DynamicMemoryUsage() counts, they hold the free ones, which nothing else counts.
    static size_t NodeRegionBytes() { return s_node_region_bytes.load(std::memory_order_relaxed); }
    void SanityCheck() const override;
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
        // Below the block the database holds, or is being brought to: what the disconnect changed is tagged
        // with the fork height, and no run may stop at a bound until a final one has carried it.
        if (height < (m_run_in_flight ? std::max(m_written_height, m_inflight_settled_height) : m_written_height)) m_fell_below_written = true;
        m_tier_height = height;
        m_confirmed_height.store(height, std::memory_order_release);
        // Nothing above the tier's block is in the database to be met (a disconnect lowers the height).
        if (m_dropped_through.load(std::memory_order_relaxed) > height) m_dropped_through.store(height, std::memory_order_relaxed);
    }

    // Connecting a block: changes with the block's identity, invisible to readers until confirmation.
    //! Open a block of `height` in the window table. Serialised by the owner. The height is above the
    //! confirmed height (that of the block confirmed last, or given with the best block), which is what
    //! tells an open block's changes from confirmed ones; only a disconnect lowers the confirmed height.
    //! `mark_fresh_spends` is given when the block, or a block that can be open below it, is checked for
    //! outpoints made twice (BIP30): the block's spend of a coin the database never held then leaves a spent
    //! marker like any other spend instead of erasing the entry, so that a lower block that creates the
    //! outpoint again finds the coin (its check fails) or the marker (its create is a conflict). Without
    //! it the lower block would find nothing and overwrite a coin that is unspent at its height.
    BlockId BeginBlock(int height, bool mark_fresh_spends = false);
    //! The coin as block `block` sees it: created long ago, by a confirmed block, by an applied block below it
    //! or by itself, and spent by none of these. An applied block above it shows it nothing: not the coins it
    //! created, and those it spent as they were (from its record). Brings old coins in from the base, clean, so that the block's spend
    //! finds them in the tier (a window block's view, BlockApplyView::SetKeepBaseReads()).
    std::optional<Coin> LookupFor(BlockId block, const COutPoint& outpoint) const;
    //! The same without bringing anything in (a block at the tip and its prefetch workers).
    std::optional<Coin> PeekCoinFor(BlockId block, const COutPoint& outpoint) const;
    /**
     * Block `block` spends `outpoint`, which it saw (LookupFor) when it checked its inputs. Another applied
     * block may have spent it since: the lower height stands. If that block is lower, this block becomes
     * the offender (its conflict count rises, the spend is not recorded); if higher, this block displaces
     * it and the other block gets the conflict. A spend the block did not see is not recorded either and
     * counts as a conflict; that includes a coin the database never held and a higher block spent first,
     * which left the tier with that spend: the owner unwinds the higher blocks and connects this one again.
     * Returns whether the spend was recorded.
     */
    bool ApplySpend(BlockId block, const COutPoint& outpoint, const Coin* known = nullptr);
    //! One of a block's changes, for ApplyBatch(): an unspent coin is the outpoint's creation, a spent one
    //! its spend.
    struct Change {
        COutPoint outpoint;
        Coin coin;
        //! For a create: a live coin under it is a BIP30 overwrite, allowed only with this, as AddCoin().
        bool possible_overwrite{false};
        //! The block was checked to create no outpoint that has a coin (BIP30): a coin met at the apply
        //! came after that check and is a conflict, also where `possible_overwrite` allows writing over one.
        bool must_be_new{false};
        //! For a spend: `coin`, spent, has the height and the coinbase flag of the coin the block validated
        //! the spend against, and another coin met at the apply is a conflict (`known` of ApplySpend()).
        bool known_coin{false};
    };
    /**
     * Block `block` makes `changes`, each as it would be made alone (a spend as ApplySpend()), but grouped by
     * shard so that each shard's lock is taken once for the block.
     */
    void ApplyBatch(BlockId block, std::vector<Change>&& changes);
    //! The coins among `outpoints` that block `viewer` sees and the tier holds, read with one hold of each
    //! shard's lock; what is missing, spent or invisible is left out (PeekCoinFor() one by one is the fallback).
    std::vector<std::pair<COutPoint, Coin>> FetchFor(BlockId viewer, std::vector<COutPoint>&& outpoints) const;
    //! The conflicts recorded against the block so far; nonzero means it must not confirm.
    uint32_t ConflictCount(BlockId block) const;
    //! The block's height, as given to BeginBlock().
    int BlockHeight(BlockId block) const;
    /**
     * A window block says which transactions it holds, before its changes are applied: until they are
     * (CreatesApplied()), a coin of one of those transaction ids is in no place a higher block could read
     * it from, and CreatesPendingBelow() says so without a lookup. The ids go into one table for all open
     * blocks, of fixed size and without a lock, where a later id may take an earlier one's place: what the
     * table says is a hint, never a fact about a coin.
     */
    void AnnounceCreates(BlockId block, std::span<const CTransactionRef> txs);
    //! The block's changes are in the store (the first ApplyBatch() of a block that announced).
    void CreatesApplied(BlockId block);
    /**
     * Whether an open block below `viewer` announced `txid` and is not applied yet. True means a lookup of
     * that transaction's outputs would find nothing now, in the tier or the database. It may be true for a
     * transaction no such block holds (two ids that share their place and tag in the table), and false for
     * one a block does hold: the caller may only take it as a reason to look again later, as a window
     * block does that defers a transaction to its continuation (ConnectBlock()), never as a coin's absence.
     */
    bool CreatesPendingBelow(BlockId viewer, const Txid& txid) const;
    /**
     * The block becomes part of the confirmed state: the confirmed height becomes its height, which is what
     * its creates and its spent markers are told from plain ones by, and the identity is freed. No entry
     * is visited but two kinds from the block's record: the spends that owe the database an erase, which
     * are filed for the next run, and the markers of coins the database never held, which are freed, and
     * only for a block that was opened with such markers (BeginBlock()): one that a block opened without
     * them took over stays until a run or a flush meets it. Blocks are confirmed from the lowest up, which
     * is asserted; that a block is opened above the confirmed height is the caller's to keep.
     */
    void ConfirmBlock(BlockId block);
    //! The block's changes are taken back from its record, the last first: its creates disappear, the coins
    //! it spent are put back as they were. The identity is freed. Blocks are unwound from the highest down
    //! (a coin a lower block created and a higher one spent goes with the higher block's unwind), and not
    //! while a run collects (StreamWriteOut(), Flush()). The order is the caller's
    //! to keep: a block whose publication failed is unwound after the blocks above it
    //! (ActivateBestChain()). A higher block's spend of a coin whose creator is gone leaves no coin behind
    //! (UnwindItem()), which is right for a coin the creator made and loses one the creator had written
    //! over.
    void UnwindBlock(BlockId block);
    //! Blocks opened and neither confirmed nor unwound.
    size_t OpenBlocks() const;

    //! The lowest height bound whose write-out frees at least `bytes` of dirty entries, or the tip height if
    //! all dirty entries together do not.
    int BoundForBytes(size_t bytes, int tip_height) const;

    //! Drop clean entries (an open block holds only dirty ones) until the tier's usage is at most `target_bytes`,
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
     *
     * One kind of write is not the outpoint's value at new_head: a confirmed create above the bound that
     * owes the database an erase is written as that erase (CollectChunk()), also where it went over an
     * unspent coin the database never had. The database then holds less than the state below the create,
     * never a coin that state does not have, and the replay re-applies the create like every block above
     * old_head. (An open block's create is written as what it went over, the value at the run's head.)
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
     * Takes shard locks, and the slot mutexes of open blocks' records (CollectRecords()), and touches nothing
     * else of the tier, so it may run on another thread than the owner's while the owner makes no confirmed
     * change. Throws what the base's WriteRun() throws;
     * the run is then abandoned (AbandonWriteOut()).
     */
    void StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes = DEFAULT_DB_CACHE_BATCH);
    //! Coins a disconnect restored that the database may never have held (see DisconnectTip()): the next
    //! run writes them first, before its marker, and a forced flush before anything (TakeRestored()). The
    //! database may hold them from now on, so none of them stays fresh: a later spend is written as an
    //! erase rather than dropped.
    void KeepWritten(std::vector<CoinWrite> restored);
    //! The restored coins not yet handed to a run, for a caller that writes them itself (a forced flush).
    std::vector<CoinWrite> TakeRestored() { return std::exchange(m_restored, {}); }
    const std::vector<CoinWrite>& Restored() const { return m_restored; }
    //! The run has landed: its settled head is the database's state from now on.
    void CommitWriteOut(const WriteOutRun& run);
    //! The run did not land (the node is going down). What it had collected counts as written in the tier and
    //! may be missing in the database, so the tier refuses every later run and flush (FlushFailed()).
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
    //! Whether a run is between BeginWriteOut() and CommitWriteOut().
    bool RunInFlight() const { return m_run_in_flight; }
    //! Whether a flush lost changes to a failed write. Nothing may be written after it (Flush()).
    bool FlushFailed() const { return m_flush_failed; }

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
        //! A spend: the entry's flags before it (Entry), `created` being its block_create. Clean and not
        //! fresh once a run wrote the coin from here (CollectRecords()), the spend then the block's marker.
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
        //! The confirmed height when the block was opened: what the block has read since is no older.
        std::atomic<int> begin_confirmed{-1};
        //! As given to BeginBlock(); the block's applier reads it.
        std::atomic<bool> mark_fresh_spends{false};
        //! From AnnounceCreates() until CreatesApplied(): the block's transaction ids in the table of
        //! pending ones stand for coins that are not in the store yet.
        std::atomic<bool> creates_pending{false};
        /**
         * What the block's changes add to the tier's dirty entries over what they add to the confirmed
         * state's (nothing, until the block is confirmed): GetDirtyCount() takes it off again. Exact
         * when blocks are unwound from the highest down.
         */
        mutable std::atomic<int64_t> dirty_delta{0};
        mutable Mutex mutex;
        uint32_t conflicts GUARDED_BY(mutex){0};
        //! Whether the record got a spend of an entry with an erase still owed (Undo::owed): ConfirmBlock()
        //! goes through the record for those only then.
        bool owed_spends GUARDED_BY(mutex){false};
        //! The block's changes in the order they were made.
        std::vector<Undo> record GUARDED_BY(mutex);
    };
    //! Which of its shard's lists an entry is on.
    enum List : uint8_t { LIST_NONE, LIST_DIRTY, LIST_CLEAN };
    /**
     * An entry is on one list of its shard, by its state: a dirty entry on the list of its change height,
     * where the write-out finds it without a scan of the map; a clean entry on the clean list, in the
     * order it became clean or was brought in, where eviction finds it.
     *
     * The flags other than `referenced`, with whether the coin is spent, are the entry's state: what
     * each flag means, which states there are and what leads from one to another is said in
     * coins_entry_machine.h, with what each change does to the height the entry is filed under. The flags
     * and the height are written by Transit() and Enter() alone. The four block flags (block_create,
     * block_spend, owed, unwritten) mean something only while the confirmed height is below the height
     * they refer to: the entry is then pending, an open block's, which the write-out passes over.
     */
    struct Entry {
        Coin coin;
        //! The height the entry is filed under, the write-out's age: the last change's as a rule, and an
        //! earlier one's where that change is owed still (coins_entry::Height).
        int32_t height{0};
        bool dirty : 1 {false};
        bool fresh : 1 {false};
        bool collected : 1 {false};
        //! A lookup found the entry since eviction last passed over it.
        bool referenced : 1 {false};
        bool block_create : 1 {false};
        bool block_spend : 1 {false};
        //! What is owed is an erase, or an unspent coin the database does not have yet, which the create's
        //! record keeps (OwedValue()).
        bool owed : 1 {false};
        //! ConfirmBlock() frees such an entry with its block if the block was opened with these markers
        //! (BeginBlock()); a run or a flush drops it otherwise.
        bool unwritten : 1 {false};
        uint8_t list : 2 {LIST_NONE};
        Node* prev{nullptr};
        Node* next{nullptr};
    };
    //! The huge page size the hint below is made for (x86-64's; elsewhere the hint is as harmless as none).
    static constexpr size_t HUGE_PAGE_BYTES{size_t{2} << 20};
    //! Memory of a multiple of HUGE_PAGE_BYTES, aligned to it, that the system is asked to back with huge
    //! pages where it can (Linux's transparent huge pages, in their "madvise" or "always" mode): a lookup
    //! lands on a random node of a large map, and with small pages misses the TLB for the bucket and again
    //! for the node. Only a hint. Freed with FreeRegion().
    static void* HintedRegion(size_t bytes);
    static inline std::atomic<size_t> s_node_region_bytes{0};
    //! The size of the regions the nodes' memory is asked for in.
    static constexpr size_t NODE_REGION_BYTES{size_t{32} << 20};
    //! From this much node memory of the process on, the next region is made ready ahead of its use
    //! (NodeRegion()); below it a region ahead would be a large share of a small store.
    static constexpr size_t NODE_REGION_AHEAD_FROM{size_t{1} << 30};
    //! A region of NODE_REGION_BYTES for nodes, as HintedRegion() gives it. The system supplies memory at
    //! its first touch, on the touching thread, which for a node is a thread connecting blocks under a
    //! shard's mutex. Once the store is large, a thread of its own therefore asks for the next region as
    //! soon as one is taken and touches all of it; a region asked for before the next is ready is supplied
    //! on the asking thread. The thread runs while there is a store: the last one to be destroyed joins it.
    static void* NodeRegion();
    struct RegionAhead;
    static RegionAhead& RegionAheadState();
    static void FreeRegion(void* region) noexcept { ::operator delete(region, std::align_val_t{HUGE_PAGE_BYTES}); }
    //! A shard's entries are inserted and erased by whichever thread holds its mutex, so a node is often freed
    //! by another thread than the one that allocated it. A thread keeps the nodes it frees for its own next
    //! insertions: a list of up to MAX_KEPT nodes and one full list beside it. A thread with two full lists
    //! gives one to the depot, and a thread with none takes one from there: a lock for every MAX_KEPT
    //! nodes, held for the exchange of a pointer.
    //!
    //! The depot carves new lists from regions of REGION_BYTES, asked for with the huge page hint
    //! (HintedRegion()). A node is never given back: one that is freed waits, with its thread or in the
    //! depot, for the next insertion, and the regions stay with the process.
    template <typename T>
    struct NodeAllocator {
        using value_type = T;
        static constexpr size_t MAX_KEPT{1024};
        static constexpr size_t REGION_BYTES{NODE_REGION_BYTES};
        //! The next full list after this one in the depot: the second word of a list's first node.
        static void*& NextList(void* list) { return static_cast<void**>(list)[1]; }
        //! Full lists, each MAX_KEPT nodes, for whichever thread next runs out, and the region new ones are
        //! carved from. Never destroyed, so that a thread may end at any time.
        struct Depot {
            Mutex mutex;
            void* lists GUARDED_BY(mutex){nullptr};
            //! The nodes of threads that ended, fewer than MAX_KEPT: a full list once there are as many.
            void* loose GUARDED_BY(mutex){nullptr};
            size_t loose_count GUARDED_BY(mutex){0};
            //! What is left of the newest region.
            std::byte* fresh GUARDED_BY(mutex){nullptr};
            size_t fresh_bytes GUARDED_BY(mutex){0};
            void Give(void* list) EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                LOCK(mutex);
                NextList(list) = lists;
                lists = list;
            }
            void GiveLoose(void* nodes) EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                LOCK(mutex);
                while (nodes) {
                    void* next{*static_cast<void**>(nodes)};
                    *static_cast<void**>(nodes) = loose;
                    loose = nodes;
                    if (++loose_count == MAX_KEPT) {
                        NextList(loose) = lists;
                        lists = std::exchange(loose, nullptr);
                        loose_count = 0;
                    }
                    nodes = next;
                }
            }
            //! A full list: one that was given, else MAX_KEPT nodes no one has used yet.
            void* Take() EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                static constexpr size_t LIST_BYTES{MAX_KEPT * sizeof(T)};
                std::byte* nodes;
                {
                    LOCK(mutex);
                    if (lists) return std::exchange(lists, NextList(lists));
                    if (fresh_bytes < LIST_BYTES) {
                        // The remainder, less than a list, is left behind.
                        fresh = static_cast<std::byte*>(NodeRegion());
                        fresh_bytes = REGION_BYTES;
                        s_node_region_bytes.fetch_add(REGION_BYTES, std::memory_order_relaxed);
                    }
                    nodes = fresh;
                    fresh += LIST_BYTES;
                    fresh_bytes -= LIST_BYTES;
                }
                // Linked outside the lock: the first touch of the memory is where the system supplies it.
                for (size_t i{0}; i < MAX_KEPT; ++i) {
                    *reinterpret_cast<void**>(nodes + i * sizeof(T)) = i + 1 < MAX_KEPT ? nodes + (i + 1) * sizeof(T) : nullptr;
                }
                return nodes;
            }
        };
        static Depot& Shared()
        {
            static Depot* const depot{new Depot};
            return *depot;
        }
        //! Whether this thread's list is destroyed: at the thread's end, and on the main thread before the
        //! static objects, which may still free nodes (a store a static test setup owns). Trivially
        //! destructible, so it can be read after that.
        static bool& KeptGone()
        {
            thread_local bool gone{false};
            return gone;
        }
        struct Kept {
            void* head{nullptr};
            size_t count{0};
            //! A full list, or none.
            void* spare{nullptr};
            ~Kept()
            {
                KeptGone() = true;
                if (spare) Shared().Give(std::exchange(spare, nullptr));
                if (head) Shared().GiveLoose(std::exchange(head, nullptr));
                count = 0;
            }
        };
        static Kept& Mine()
        {
            static_assert(sizeof(T) >= 2 * sizeof(void*) && sizeof(T) % alignof(T) == 0 && alignof(T) <= HUGE_PAGE_BYTES);
            thread_local Kept kept;
            return kept;
        }
        NodeAllocator() = default;
        template <typename U>
        NodeAllocator(const NodeAllocator<U>&) noexcept {}
        T* allocate(size_t n)
        {
            Assert(n == 1);
            if (KeptGone()) {
                // One node of a list of the depot's (which is never destroyed), the rest back to it.
                void* node{Shared().Take()};
                Shared().GiveLoose(std::exchange(*static_cast<void**>(node), nullptr));
                return static_cast<T*>(node);
            }
            Kept& kept{Mine()};
            if (!kept.head) {
                // The full list beside, else one of the depot's.
                kept.head = kept.spare ? std::exchange(kept.spare, nullptr) : Shared().Take();
                kept.count = MAX_KEPT;
            }
            void* node{kept.head};
            kept.head = *static_cast<void**>(node);
            --kept.count;
            return static_cast<T*>(node);
        }
        void deallocate(T* p, size_t n) noexcept
        {
            Assert(n == 1);
            if (KeptGone()) {
                *reinterpret_cast<void**>(p) = nullptr;
                Shared().GiveLoose(p);
                return;
            }
            Kept& kept{Mine()};
            if (kept.count == MAX_KEPT) {
                // The full list goes beside, the one that was there to the depot, and a new one begins.
                if (kept.spare) Shared().Give(kept.spare);
                kept.spare = std::exchange(kept.head, nullptr);
                kept.count = 0;
            }
            *reinterpret_cast<void**>(p) = kept.head;
            kept.head = p;
            ++kept.count;
        }
        friend bool operator==(const NodeAllocator&, const NodeAllocator&) noexcept { return true; }
    };
    //! The salted hash of an outpoint, one salt for all shards: a batch hashes its keys before it takes a shard.
    static uint64_t KeyHash(const COutPoint& outpoint) noexcept
    {
        static const SaltedCoinsCacheHasher hasher;
        return hasher(outpoint);
    }
    /**
     * A shard's table: chained, with nodes that never move (the lists and a run in flight point at them),
     * of the std::unordered_map interface as far as the store uses it. The bucket array is a power of two
     * and a chain ends at a null pointer. A caller with many keys for one shard hashes them first and asks
     * for their buckets and nodes ahead (Prefetch(), PrefetchNode()).
     */
    class Map
    {
    public:
        using value_type = Node;
        struct Cell {
            Cell* chain;
            Node node;
        };
        class iterator
        {
        public:
            iterator() = default;
            iterator(const Map* map, size_t bucket, Cell* cell) : m_map{map}, m_bucket{bucket}, m_cell{cell} {}
            Node& operator*() const { return m_cell->node; }
            Node* operator->() const { return &m_cell->node; }
            bool operator==(const iterator& other) const { return m_cell == other.m_cell; }
            iterator& operator++()
            {
                m_cell = m_cell->chain;
                while (!m_cell && ++m_bucket < m_map->m_bucket_count) m_cell = m_map->m_buckets[m_bucket];
                return *this;
            }

        private:
            friend class Map;
            const Map* m_map{nullptr};
            size_t m_bucket{0};
            Cell* m_cell{nullptr};
        };

        Map() = default;
        Map(const Map&) = delete;
        Map& operator=(const Map&) = delete;
        ~Map()
        {
            for (size_t b{0}; b < m_bucket_count; ++b) {
                for (Cell* cell{m_buckets[b]}; cell;) {
                    Cell* next{cell->chain};
                    Free(cell);
                    cell = next;
                }
            }
        }

        size_t size() const { return m_size; }
        size_t bucket_count() const { return m_bucket_count; }
        iterator end() const { return {}; }
        iterator begin() const
        {
            for (size_t b{0}; b < m_bucket_count; ++b) {
                if (m_buckets[b]) return {this, b, m_buckets[b]};
            }
            return {};
        }
        //! Ask for the bucket of a hash ahead of its probe.
        void Prefetch(uint64_t hash) const
        {
            if (m_bucket_count != 0) Ahead(&m_buckets[hash & (m_bucket_count - 1)]);
        }
        //! Ask for the first node of the bucket's chain, both of its cache lines. Reads the bucket.
        void PrefetchNode(uint64_t hash) const
        {
            if (m_bucket_count == 0) return;
            if (const Cell* cell{m_buckets[hash & (m_bucket_count - 1)]}) {
                Ahead(cell);
                Ahead(reinterpret_cast<const char*>(cell) + 64);
            }
        }
        //! Ask for every item's bucket, then every chain's first node, before the first probe waits for one.
        void PrefetchAll(std::span<const uint64_t> hashes, std::span<const uint32_t> items) const
        {
            for (const uint32_t i : items) Prefetch(hashes[i]);
            for (const uint32_t i : items) PrefetchNode(hashes[i]);
        }
        iterator find(const COutPoint& key) const { return find(key, KeyHash(key)); }
        iterator find(const COutPoint& key, uint64_t hash) const
        {
            if (m_bucket_count == 0) return {};
            const size_t bucket{hash & (m_bucket_count - 1)};
            for (Cell* cell{m_buckets[bucket]}; cell; cell = cell->chain) {
                if (cell->node.first == key) return {this, bucket, cell};
            }
            return {};
        }
        std::pair<iterator, bool> try_emplace(const COutPoint& key) { return try_emplace(key, KeyHash(key)); }
        std::pair<iterator, bool> try_emplace(const COutPoint& key, uint64_t hash)
        {
            if (const iterator it{find(key, hash)}; it != end()) return {it, false};
            if (m_size == m_bucket_count) Rebucket(m_bucket_count == 0 ? MIN_BUCKETS : m_bucket_count * 2);
            const size_t bucket{hash & (m_bucket_count - 1)};
            Cell* cell{NodeAllocator<Cell>{}.allocate(1)};
            new (&cell->node) Node(std::piecewise_construct, std::forward_as_tuple(key), std::tuple<>());
            cell->chain = m_buckets[bucket];
            m_buckets[bucket] = cell;
            ++m_size;
            return {iterator{this, bucket, cell}, true};
        }
        iterator erase(iterator it)
        {
            iterator next{it};
            ++next;
            Cell** link{&m_buckets[it.m_bucket]};
            while (*link != it.m_cell) link = &(*link)->chain;
            *link = it.m_cell->chain;
            Free(it.m_cell);
            --m_size;
            return next;
        }
        size_t erase(const COutPoint& key)
        {
            const iterator it{find(key)};
            if (it == end()) return 0;
            erase(it);
            return 1;
        }
        //! Only rehash(0) is used: the smallest array for the entries there are.
        void rehash(size_t)
        {
            size_t count{m_size == 0 ? 0 : MIN_BUCKETS};
            while (count < m_size) count *= 2;
            if (count != m_bucket_count) Rebucket(count);
        }

    private:
        static constexpr size_t MIN_BUCKETS{16};
        //! A hint only: without the builtin the probe waits for its memory.
        static void Ahead(const void* p)
        {
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(p);
#else
            (void)p;
#endif
        }
        static void Free(Cell* cell)
        {
            cell->node.~Node();
            NodeAllocator<Cell>{}.deallocate(cell, 1);
        }
        void Rebucket(size_t count)
        {
            Buckets buckets{nullptr, BucketsDeleter{count * sizeof(Cell*) >= HUGE_PAGE_BYTES}};
            if (count != 0) {
                // An array of a huge page or more (a power of two, so a multiple of it) asks for huge pages.
                void* array{buckets.get_deleter().hinted ? HintedRegion(count * sizeof(Cell*)) : ::operator new(count * sizeof(Cell*))};
                buckets.reset(new (array) Cell*[count]());
            }
            for (size_t b{0}; b < m_bucket_count; ++b) {
                for (Cell* cell{m_buckets[b]}; cell;) {
                    Cell* next{cell->chain};
                    const size_t bucket{KeyHash(cell->node.first) & (count - 1)};
                    cell->chain = buckets[bucket];
                    buckets[bucket] = cell;
                    cell = next;
                }
            }
            m_buckets = std::move(buckets);
            m_bucket_count = count;
        }

        struct BucketsDeleter {
            bool hinted;
            void operator()(Cell** array) const noexcept
            {
                if (hinted) {
                    FreeRegion(array);
                } else {
                    ::operator delete(array);
                }
            }
        };
        using Buckets = std::unique_ptr<Cell*[], BucketsDeleter>;
        Buckets m_buckets{nullptr, BucketsDeleter{false}};
        size_t m_bucket_count{0};
        size_t m_size{0};
    };
    //! The dirty entries of one span of change heights in a shard.
    struct Bucket {
        //! What writing them out frees: DirtyBytes() of each.
        int64_t bytes{0};
        Node* head{nullptr};
    };
    /**
     * What a shard's mutex is built on. A shard is held for a few entries and free again well within the
     * time it takes to put a waiting thread to sleep and to wake it up, so a waiter retries for about that
     * long before it blocks. (A write-out holds a shard for longer; its waiters block once the retries
     * are spent.)
     */
    class ShardLock
    {
    public:
        void lock();
        bool try_lock() { return m_mutex.try_lock(); }
        void unlock() { m_mutex.unlock(); }

    private:
        std::mutex m_mutex;
    };
#if defined(DEBUG_LOCKORDER) || defined(DEBUG_LOCKCONTENTION)
    //! The lock order checks and the contention log know the standard mutexes only: such a build has no retries.
    using ShardMutex = Mutex;
#else
    using ShardMutex = AnnotatedMixin<ShardLock>;
#endif
    //! On cache lines of its own: a shard is written by whichever thread holds it, and its neighbours by others.
    struct alignas(64) Shard {
        mutable ShardMutex mutex;
        Map map GUARDED_BY(mutex);
        //! The shard's share of the tier's totals (see Totals): its dirty entries, and its map's size and
        //! bucket count as last counted (Resized()).
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
     * for every block it publishes, and every change of an entry moves them. They are kept as sums of
     * deltas in stripes, a thread adding to the stripe it was dealt and each stripe on a cache line of
     * its own, so that a query reads the stripes and neither locks nor reads a shard. One stripe means
     * nothing (what one thread adds another takes away); the sum is the total, exact when no change is
     * under way.
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
    //! The region made ready ahead of its use, as DynamicMemoryUsage() counts it: memory the system has
    //! supplied and no node lives in yet. A bound, not a reading: NODE_REGION_BYTES for a store whose own
    //! nodes come to NODE_REGION_AHEAD_FROM, nothing for a smaller store, whose budget may be less than a
    //! region (a chainstate beside a snapshot's). The process may hold a region all the same while this
    //! counts nothing (NodeRegion()). Not counted, here or elsewhere: the free nodes (NodeAllocator), and
    //! the part of the newest region no list was carved from, which the system has not supplied.
    size_t RegionAheadBytes() const;
    /**
     * How many change heights share a list of dirty entries, as a power of two. While nothing is written
     * out the dirty entries are the coins of every height since the last write, a few to a shard for each:
     * a list for every height would be an allocation of its own for nearly every dirty coin. An entry
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
    using State = coins_entry::State;
    using Event = coins_entry::Event;
    //! The entry's state: its flags, and whether its coin is spent.
    static State StateOf(const Entry& entry)
    {
        using namespace coins_entry;
        return static_cast<State>((entry.dirty ? DIRTY : 0) | (entry.fresh ? FRESH : 0) | (entry.collected ? COLLECTED : 0) |
                                  (entry.block_create ? BLOCK_CREATE : 0) | (entry.block_spend ? BLOCK_SPEND : 0) |
                                  (entry.owed ? OWED : 0) | (entry.unwritten ? UNWRITTEN : 0) | (entry.coin.IsSpent() ? SPENT : 0));
    }
    //! Put the entry in the state `to`, which the machine answered and which the entry's coin agrees
    //! with, and file it under `height`, which the machine answered as well. An event the machine refuses
    //! is a defect of the store's logic, and stops the node here. The dirty lists are by height: an entry
    //! changes its height off its list (Detach()).
    static void Enter(Entry& entry, State to, std::optional<int> height)
    {
        using namespace coins_entry;
        Assert(to != REFUSED);
        Assert(static_cast<bool>(to & SPENT) == entry.coin.IsSpent());
        Assert(height.has_value());
        Assert(*height == entry.height || entry.list == LIST_NONE);
        entry.height = *height;
        entry.dirty = to & DIRTY;
        entry.fresh = to & FRESH;
        entry.collected = to & COLLECTED;
        entry.block_create = to & BLOCK_CREATE;
        entry.block_spend = to & BLOCK_SPEND;
        entry.owed = to & OWED;
        entry.unwritten = to & UNWRITTEN;
    }
    //! `event` happened to the entry at `height` (the change's, the block's, the coin's or a record's, as
    //! the event has it). The entry was in `from` before and has its coin as the event leaves it. With the
    //! entry off its list where the event changes which list it belongs on (Detach()).
    static void Transit(Entry& entry, State from, Event event, int height)
    {
        using namespace coins_entry;
        Enter(entry, Next(from, event), NextHeight(HeightRule(from, event), from, entry.height, height));
    }
    //! The same for an event that has no height and leaves the entry where it is filed.
    static void Transit(Entry& entry, State from, Event event)
    {
        Assert(coins_entry::HeightRule(from, event) == coins_entry::Height::Keep);
        Enter(entry, coins_entry::Next(from, event), entry.height);
    }
    //! The same for events that leave the coin spent or unspent as it was.
    static void Transit(Entry& entry, Event event, int height) { Transit(entry, StateOf(entry), event, height); }
    static void Transit(Entry& entry, Event event) { Transit(entry, StateOf(entry), event); }
    //! Collect the shard's dirty entries at or below the run's bound into `writes` and `nodes` until
    //! `size`, about what the collected changes come to in a database batch, reaches `limit`; note the
    //! oldest change height left behind. An open block's entries are passed over, except for the change
    //! one of its creates went over (Entry::owed: an erase, or the coin OwedValue() gives), which is written.
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
    bool SpendEntry(Shard& shard, Map::iterator it, Coin* moveto) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! The entry's coin as a copy, bringing it in from the base on a miss; nullopt if absent or spent.
    std::optional<Coin> Lookup(const COutPoint& outpoint) const;
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
    int ViewerHeight(BlockId viewer) const { return viewer == NO_BLOCK ? -1 : BlockHeight(viewer); }
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
    //! What a run writes for an open block's create that went over an erase the database is owed, with the
    //! shard held: the unspent coin the create went over where it kept one (a coinbase at the chain's two
    //! BIP30 duplicates, over a confirmed coin not yet written), the coin's value in the confirmed state;
    //! else the erase.
    std::optional<Coin> OwedValue(int creator_height, const COutPoint& outpoint) const;
    //! Append `undo` to the block's record and add `delta` to its dirty delta, emptying both. With the
    //! shard of the changes still held: who finds their traces in the shard finds the record complete.
    static void Record(WindowBlock& mine, std::vector<Undo>& undo, int64_t& delta);
    /**
     * Visit the shards of a block's items, `shard_of[i]` being item i's: `work(shard, items)` runs once
     * for every shard that has any, with its lock held and the indices of its items in their order. The
     * walk starts at a shard of the block's own and passes over a shard another thread holds, coming back
     * for those at the end: there is no order between shards to keep.
     */
    template <typename Work>
    void VisitShards(BlockId block, const std::vector<uint32_t>& shard_of, Work work) const;
    //! Erase an entry, keeping the shard's lists and counters right.
    void EraseEntry(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! The same for an entry reached over a list, erased by its key: a cell has no link back, so the erase
    //! walks the chain from its bucket either way.
    void EraseEntry(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Erase an entry that is off its list and whose coin's usage is taken off the books already.
    void EraseDetached(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    struct Wipe;
    struct WipeWorker;
    //! A flush's worker: takes its shards from the highest down, and writes what it took out of them.
    void RunWipeWorker(Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    //! Take the shard's entries out of it, its changes into the worker's batch in the order of their
    //! keys. Returns what the shard's changes come to as a database batch.
    uint64_t WipeShard(Shard& shard, WipeWorker& worker) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Write the worker's batch and unlock the shards it emptied: their changes are in the database.
    void WriteWipeBatch(const Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    //! The map's size or bucket count may have changed: count the difference into the totals.
    void Resized(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! A create of the block at `height` (ApplyBatch()) with the shard held. The change goes to `undo` and what it
    //! adds to the dirty entries to `delta`, for the caller to hand to Record() before it lets go of the
    //! shard. False: not made, a conflict for the block (the outpoint is another open block's to settle).
    bool CreateLocked(Shard& shard, int height, const COutPoint& outpoint, uint64_t hash, Coin&& coin, bool possible_overwrite, bool must_be_new, bool base_unseen, std::vector<Undo>& undo, int64_t& delta) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    enum class SpendResult { DONE, CONFLICT, MISS };
    //! ApplySpend() for the block at `height` with the shard held, `undo` and `delta` as for
    //! CreateLocked(). MISS: the entry is not in the tier and `from_base` was not given; the caller reads
    //! the base with the shard unlocked and comes back with the coin. The caller counts a CONFLICT.
    SpendResult SpendLocked(Shard& shard, int height, bool mark_fresh, const COutPoint& outpoint, uint64_t hash, const Coin* from_base, const Coin* known, std::vector<Undo>& undo, int64_t& delta) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! One item of the record of the block at `height`, which is being unwound, taken back with the shard held.
    void UnwindItem(Shard& shard, int height, Undo&& item) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    /**
     * The shard of an outpoint, by the leading bytes of its txid as the database's key begins with them:
     * the shards are consecutive ranges of the database's keys in the order of their index
     * (SetBaseWrites()). A transaction's outputs share a shard. Whoever makes a transaction can pick its
     * shard by grinding, which buys contention on that shard and nothing else: an entry's place in the
     * shard's map is by the salted hash.
     */
    size_t ShardIndex(const COutPoint& outpoint) const { return (uint64_t{ReadBE32(outpoint.hash.ToUint256().data())} * m_shards.size()) >> 32; }

    /**
     * The change height an unspent coin arriving outside a block identity is tagged with. A coin
     * restored by a disconnect (the change height is then the fork height) that was created above the
     * settled height is not in the database. It keeps its creation height, below the fork or above it:
     * a run bounded below its creation leaves it and the settled head below its creating block, which a
     * replay then applies again, and a run bounded at or above its creation writes it. Tagged with the
     * fork height, a run bounded between the two would settle the database above its creation without
     * it. Any other coin takes the change height.
     */
    int TagFor(const Coin& coin) const
    {
        const int settled{m_run_in_flight ? m_inflight_settled_height : m_written_height};
        return coin.nHeight > settled ? static_cast<int>(coin.nHeight) : m_change_height;
    }

    mutable std::vector<Shard> m_shards;
    mutable std::array<Totals, TOTALS_STRIPES> m_totals;
    mutable uint256 m_tier_block_hash;
    int m_change_height{0};
    int m_tier_height{-1};
    uint256 m_written_head;
    int m_written_height{-1};
    bool m_run_in_flight{false};
    //! A flush lost changes to a failed write (Flush()). The run it began never ends.
    bool m_flush_failed{false};
    //! The settled height of the run in flight: what the database holds once it lands.
    int m_inflight_settled_height{-1};
    //! The tier's block has been below the written head since the last final run began (a disconnect):
    //! every run is final, whatever its caller asked for (BeginWriteOut()).
    bool m_fell_below_written{false};
    //! Restored coins for the next run (KeepWritten()); the owner's alone.
    std::vector<CoinWrite> m_restored;

    //! The window table, indexed by BlockId; slot 0 is NO_BLOCK. Slots are claimed by one thread at a
    //! time (BeginBlock, serialised by the owner); a slot's status and height are read lock-free, the rest
    //! under the slot's own mutex.
    std::array<WindowBlock, MAX_WINDOW_SLOTS> m_window;
    //! The height of the block confirmed last (or given with the best block): a block's create or spent
    //! marker above it is an open block's. Read by every lookup that meets one, without a lock.
    std::atomic<int> m_confirmed_height{-1};
    //! The highest height whose confirmed changes a run or a flush may have written, and the tier dropped
    //! since: stored before the first entry goes. A block opened below it cannot take the absence of an
    //! entry for the absence of a coin (CreateLocked()).
    std::atomic<int> m_dropped_through{-1};
    //! The slots not free, for the readers' check that there is no record to look into.
    std::atomic<int> m_open_slots{0};
    /**
     * The transaction ids the open blocks announced (AnnounceCreates()). An id has one place, by some of
     * its bits, and an entry is a tag from other bits of the id with the height and the slot of the block
     * that announced it; zero is empty. Entries are never removed: one is void once its block's slot is
     * free, at another height or applied. 4 MiB, allocated when the first block announces, and not counted
     * as cache: it holds no coin and does not grow.
     */
    static constexpr size_t PENDING_TXID_PLACES{size_t{1} << 19};
    std::once_flag m_pending_txids_once;
    std::unique_ptr<std::atomic<uint64_t>[]> m_pending_txids_owner; //!< set once, under m_pending_txids_once
    std::atomic<std::atomic<uint64_t>*> m_pending_txids{nullptr};
    //! See SetBaseWrites().
    uint64_t m_batch_bytes{DEFAULT_DB_CACHE_BATCH};
    bool m_base_concurrent{false};
    //! See SetFlushHelpers().
    std::shared_ptr<ThreadPool> m_flush_helpers;
};

/**
 * The view a block is connected into: a CoinsViewOverlay (prefetch and the block's own changes for the
 * core's lookups and references) whose changes go into the store under the block's identity
 * (ApplyChanges()), where other appliers see them. The view's reset unwinds a block that was neither
 * confirmed nor unwound.
 */
class BlockApplyView final : public CoinsViewOverlay
{
public:
    BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic = false);
    ~BlockApplyView() override;

    //! Open `height`'s block in the store; the changes that follow carry its identity. See
    //! CoinsStore::BeginBlock() for `mark_fresh_spends`.
    void BeginBlock(int height, bool mark_fresh_spends = false);
    CoinsStore::BlockId Block() const { return m_block; }
    bool HasBlock() const { return m_block != CoinsStore::NO_BLOCK; }
    //! The block becomes confirmed state; see CoinsStore::ConfirmBlock(). The view is reset.
    void Confirm();
    //! The block's changes are taken back; see CoinsStore::UnwindBlock(). The view is reset.
    void Unwind();
    uint32_t Conflicts() const;
    //! The block's validation checks that none of its outputs has a coin already (BIP30); see
    //! CoinsStore::Change::must_be_new. Set before the changes are applied.
    void SetCreatesMustBeNew(bool must_be_new) { m_creates_must_be_new = must_be_new; }
    //! Whether a coin the view reads from the database stays in the store, clean (CoinsStore::LookupFor()),
    //! so that the apply of its spend does not read the database a second time. For a window block. A block
    //! at the tip reads without it: one that fails leaves none of its inputs in the cache.
    //! Set before the block's first lookup; the view's reset clears it.
    void SetKeepBaseReads(bool keep) { m_keep_base_reads = keep; }
    //! See CoinsStore::AnnounceCreates(): called with the block before its validation, by a window block.
    void AnnounceCreates(const CBlock& block);
    //! See CoinsStore::CreatesPendingBelow(), for the view's block.
    bool CreatesPendingBelow(const Txid& txid) const { return HasBlock() && m_store.CreatesPendingBelow(m_block, txid); }

    //! Bring the block's inputs that the tier holds into the view, with one hold of each shard's lock.
    void Prefetch(const CBlock& block);
    //! Apply the view's changes so far to the store under the block's identity, grouped by shard, and drop
    //! them from the view; the next lookups read the store, where they are visible to this block. Called by
    //! the block's worker once validated (and again after a continuation), so that the work is off the
    //! publisher; Confirm() applies what is left.
    void ApplyChanges();
    //! As the cache's, and the spend left in the view keeps the height and the coinbase flag of its coin.
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    //! As Confirm().
    void Flush(bool reallocate_cache = true) override;

protected:
    std::optional<Coin> ReadBase(const COutPoint& outpoint) const override;
    void Reset() noexcept override;

private:
    CoinsStore& m_store;
    CoinsStore::BlockId m_block{CoinsStore::NO_BLOCK};
    bool m_creates_must_be_new{false};
    bool m_keep_base_reads{false};
};

#endif // BITCOIN_COINSSTORE_H
