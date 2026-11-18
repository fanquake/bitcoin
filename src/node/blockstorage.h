// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCKSTORAGE_H
#define BITCOIN_NODE_BLOCKSTORAGE_H

#include <attributes.h>
#include <chain.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <kernel/blockmanager_opts.h>
#include <kernel/effects.h>
#include <kernel/chainparams.h>
#include <kernel/cs_main.h>
#include <kernel/messagestartchars.h>
#include <node/block_facts.h>
#include <node/durability_machine.h>
#include <primitives/block.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/hasher.h>
#include <util/obfuscation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BlockValidationState;
class CBlockUndo;
class Chainstate;
class ChainstateManager;
namespace Consensus {
struct Params;
}
namespace util {
class SignalInterrupt;
} // namespace util

namespace kernel {
class CBlockFileInfo
{
public:
    uint32_t nBlocks{};      //!< number of blocks stored in file
    uint64_t nSize{};        //!< number of used bytes of block file
    uint64_t nUndoSize{};    //!< number of used bytes in the undo file
    uint32_t nHeightFirst{}; //!< lowest height of block in file
    uint32_t nHeightLast{};  //!< highest height of block in file
    uint64_t nTimeFirst{};   //!< earliest time of block in file
    uint64_t nTimeLast{};    //!< latest time of block in file

    SERIALIZE_METHODS(CBlockFileInfo, obj)
    {
        READWRITE(VARINT(obj.nBlocks));
        READWRITE(VARINT(obj.nSize));
        READWRITE(VARINT(obj.nUndoSize));
        READWRITE(VARINT(obj.nHeightFirst));
        READWRITE(VARINT(obj.nHeightLast));
        READWRITE(VARINT(obj.nTimeFirst));
        READWRITE(VARINT(obj.nTimeLast));
    }

    CBlockFileInfo() = default;

    std::string ToString() const;

    /** update statistics (does not update nSize) */
    void AddBlock(unsigned int nHeightIn, uint64_t nTimeIn)
    {
        if (nBlocks == 0 || nHeightFirst > nHeightIn)
            nHeightFirst = nHeightIn;
        if (nBlocks == 0 || nTimeFirst > nTimeIn)
            nTimeFirst = nTimeIn;
        nBlocks++;
        if (nHeightIn > nHeightLast)
            nHeightLast = nHeightIn;
        if (nTimeIn > nTimeLast)
            nTimeLast = nTimeIn;
    }
};

/** Access to the block database (blocks/index/) */
class BlockTreeDB : public CDBWrapper
{
public:
    using CDBWrapper::CDBWrapper;
    void WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    //! Serialise the batch under the lock, commit it (synced) without it.
    std::unique_ptr<CDBBatch> PrepareBatch(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    void CommitBatch(CDBBatch& batch);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo& info);
    bool ReadLastBlockFile(int& nFile);
    void WriteReindexing(bool fReindexing);
    void ReadReindexing(bool& fReindexing);
    void WriteFlag(const std::string& name, bool fValue);
    bool ReadFlag(const std::string& name, bool& fValue);
    bool LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex, const util::SignalInterrupt& interrupt)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};
} // namespace kernel

namespace node {
using kernel::CBlockFileInfo;
using kernel::BlockTreeDB;

/** The pre-allocation chunk size for blk?????.dat files (since 0.8) */
inline constexpr uint64_t BLOCKFILE_CHUNK_SIZE{16_MiB};
/** The pre-allocation chunk size for rev?????.dat files (since 0.8) */
inline constexpr uint64_t UNDOFILE_CHUNK_SIZE{1_MiB};
/** The maximum size of a blk?????.dat file (since 0.8) */
inline constexpr uint64_t MAX_BLOCKFILE_SIZE{128_MiB};

/** Size of header written by WriteBlock before a serialized CBlock (8 bytes) */
inline constexpr uint64_t STORAGE_HEADER_BYTES{std::tuple_size_v<MessageStartChars> + sizeof(uint32_t)};

/** Total overhead when writing undo data: header (8 bytes) plus checksum (32 bytes) */
inline constexpr uint64_t UNDO_DATA_DISK_OVERHEAD{STORAGE_HEADER_BYTES + uint256::size()};

// Because validation code takes pointers to the map's CBlockIndex objects, if
// we ever switch to another associative container, we need to either use a
// container that has stable addressing (true of all std associative
// containers), or make the key a `std::unique_ptr<CBlockIndex>`
using BlockMap = std::unordered_map<uint256, CBlockIndex, BlockHasher>;

struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
    using is_transparent = void;
};

struct CBlockIndexHeightOnlyComparator {
    /* Only compares the height of two block indices, doesn't try to tie-break */
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
};

struct PruneLockInfo {
    /// Height of earliest block that should be kept and not pruned
    int height_first{std::numeric_limits<int>::max()};
};

enum BlockfileType {
    // Values used as array indexes - do not change carelessly.
    NORMAL = 0,
    ASSUMED = 1,
    NUM_TYPES = 2,
};

std::ostream& operator<<(std::ostream& os, const BlockfileType& type);

struct BlockfileCursor {
    // The latest blockfile number.
    int file_num{0};

    // Track the height of the highest block in file_num whose undo
    // data has been written. Block data is written to block files in download
    // order, but is written to undo files in validation order, which is
    // usually in order by height. To avoid wasting disk space, undo files will
    // be trimmed whenever the corresponding block file is finalized and
    // the height of the highest block written to the block file equals the
    // height of the highest block written to the undo file. This is a
    // heuristic and can sometimes preemptively trim undo files that will write
    // more data later, and sometimes fail to trim undo files that can't have
    // more data written later.
    int undo_height{0};
};

std::ostream& operator<<(std::ostream& os, const BlockfileCursor& cursor);

enum class ReadRawError {
    IO,
    BadPartRange,
};

/**
 * Maintains a tree of blocks (stored in `m_block_index`) which is consulted
 * to determine where the most-work tip is.
 *
 * This data is used mostly in `Chainstate` - information about, e.g.,
 * candidate tips is not maintained here.
 */
class BlockManager
{
    friend Chainstate;
    friend ChainstateManager;

private:
    const CChainParams& GetParams() const { return m_opts.chainparams; }
    const Consensus::Params& GetConsensus() const { return m_opts.chainparams.GetConsensus(); }
    /**
     * Load the blocktree off disk and into memory. Populate certain metadata
     * per index entry (nStatus, nChainWork, nTimeMax, etc.) as well as peripheral
     * collections like m_dirty_blockindex.
     */
    bool LoadBlockIndex(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Return false if block file or undo file flushing fails. */
    [[nodiscard]] bool FlushBlockFile(int blockfile_num, bool fFinalize, bool finalize_undo) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Trim a block file the write position has left, and its undo file where that is finished too, without a
     *  sync: both stay marked unsynced until whatever records their blocks in the block index syncs them. */
    void TrimBlockFile(int blockfile_num, bool trim_undo) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Return false if undo file flushing fails. */
    [[nodiscard]] bool FlushUndoFile(int block_file, bool finalize = false) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Helper function performing various preparations before a block can be saved to disk:
     * Returns the correct position for the block to be saved, which may be in the current or a new
     * block file depending on nAddSize. May flush the previous blockfile to disk if full, updates
     * blockfile info, and checks if there is enough disk space to save the block.
     *
     * The nAddSize argument passed to this function should include not just the size of the serialized CBlock, but also the size of
     * separator fields (STORAGE_HEADER_BYTES).
     */
    [[nodiscard]] FlatFilePos FindNextBlockPos(uint64_t nAddSize, unsigned int nHeight, uint64_t nTime) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    [[nodiscard]] bool FlushChainstateBlockFile(int tip_height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    [[nodiscard]] bool FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, uint64_t nAddSize) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    AutoFile OpenUndoFile(const FlatFilePos& pos, bool fReadOnly = false) const;

    /* Calculate the block/rev files to delete based on height specified by user with RPC command pruneblockchain */
    void FindFilesToPruneManual(
        std::set<int>& setFilesToPrune,
        int nManualPruneHeight,
        const Chainstate& chain);

    /**
     * Prune block and undo files (blk???.dat and rev???.dat) so that the disk space used is less than a user-defined target.
     * The user sets the target (in MB) on the command line or in config file.  This will be run on startup and whenever new
     * space is allocated in a block or undo file, staying below the target. Changing back to unpruned requires a reindex
     * (which in this case means the blockchain must be re-downloaded.)
     *
     * Pruning functions are called from FlushStateToDisk when the m_check_for_pruning flag has been set.
     * Block and undo files are deleted in lock-step (when blk00003.dat is deleted, so is rev00003.dat.)
     * Pruning cannot take place until the longest chain is at least a certain length (CChainParams::nPruneAfterHeight).
     * Pruning will never delete a block within a defined distance (currently 288) from the active chain's tip.
     * The block index is updated by unsetting HAVE_DATA and HAVE_UNDO for any blocks that were stored in the deleted files.
     * A db flag records the fact that at least some block files have been pruned.
     *
     * @param[out]   setFilesToPrune   The set of file indices that can be unlinked will be returned
     * @param        last_prune        The last height we're able to prune: what the prune locks allow, and
     *                                 never above the block the coins database was written at (the caller's
     *                                 bound, FlushStateToDisk()). A file a block write in flight still
     *                                 goes to is left out whatever its heights; the queued undo writes
     *                                 are the caller's to settle first.
     */
    void FindFilesToPrune(
        std::set<int>& setFilesToPrune,
        int last_prune,
        const Chainstate& chain,
        ChainstateManager& chainman);

    //! Whether automatic pruning would take files now: the conditions under which FindFilesToPrune() looks
    //! for any (a prune target, a chain past the height pruning starts at, the block files at the target).
    bool AtPruneTarget(const Chainstate& chain, ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Whether a block file that holds data ends above the first height and at or below the second: a file
    //! pruning could take if it were allowed up to the second height and is not while held at the first.
    bool HasBlockFileEndingWithin(int above, int upto) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Since assumedvalid chainstates may be syncing a range of the chain that is very
    //! far away from the normal/background validation process, we should segment blockfiles
    //! for assumed chainstates. Otherwise, we might have wildly different height ranges
    //! mixed into the same block files, which would impair our ability to prune
    //! effectively.
    //!
    //! This data structure maintains separate blockfile number cursors for each
    //! BlockfileType. The ASSUMED state is initialized, when necessary, in FindNextBlockPos().
    //!
    //! The first element is the NORMAL cursor, second is ASSUMED.
    std::array<std::optional<BlockfileCursor>, BlockfileType::NUM_TYPES>
        m_blockfile_cursors GUARDED_BY(::cs_main) = {
            BlockfileCursor{},
            std::nullopt,
    };
    int MaxBlockfileNum() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        static const BlockfileCursor empty_cursor;
        const auto& normal = m_blockfile_cursors[BlockfileType::NORMAL].value_or(empty_cursor);
        const auto& assumed = m_blockfile_cursors[BlockfileType::ASSUMED].value_or(empty_cursor);
        return std::max(normal.file_num, assumed.file_num);
    }

    /** Global flag to indicate we should check to see if there are
     *  block/undo files that should be deleted.  Set on startup
     *  or if we allocate more file space when we're in prune mode
     */
    bool m_check_for_pruning = false;

    const bool m_prune_mode;

    const Obfuscation m_obfuscation;

    /**
     * Map from external index name to oldest block that must not be pruned.
     *
     * @note Internally, only blocks at height (height_first - PRUNE_LOCK_BUFFER - 1) and
     * below will be pruned, but callers should avoid assuming any particular buffer size.
     */
    std::unordered_map<std::string, PruneLockInfo> m_prune_locks GUARDED_BY(::cs_main);

    BlockfileType BlockfileTypeForHeight(int height);

    const kernel::BlockManagerOpts m_opts;

    const FlatFileSeq m_block_file_seq;
    const FlatFileSeq m_undo_file_seq;

protected:
    std::vector<CBlockFileInfo> m_blockfile_info;

    /** Dirty block index entries. */
    std::set<CBlockIndex*> m_dirty_blockindex;

    /** Dirty block file entries. */
    std::set<int> m_dirty_fileinfo;

    /**
     * Block files and undo files written to since their last successful sync.
     * A file enters its set when a position in it is claimed and leaves it
     * when it is synced or pruned. See node::DurabilityState.
     */
    std::set<int> m_unsynced_block_files GUARDED_BY(::cs_main);
    std::set<int> m_unsynced_undo_files GUARDED_BY(::cs_main);
    //! A flush of a block file or undo file failed: at the sync, before it (opening or truncating the
    //! file) or after it (closing the file). Never reset (SyncFailed()). Set wherever a file is synced,
    //! under cs_main or on the effects executor.
    std::atomic<bool> m_sync_failed{false};

    /** Block files with a block write in flight outside cs_main, by the number of such writes. Pruning
     *  leaves them alone. */
    std::map<int, int> m_blockfiles_being_written GUARDED_BY(::cs_main);

public:
    using Options = kernel::BlockManagerOpts;
    using ReadRawBlockResult = util::Expected<std::vector<std::byte>, ReadRawError>;

    //! `ledger` is the node's ledger of memory in flight (util/byte_ledger.h), the chainstate manager's; a
    //! block manager made without one keeps its own.
    explicit BlockManager(const util::SignalInterrupt& interrupt, Options opts, util::ByteLedger* ledger = nullptr);
    ~BlockManager();

    const util::SignalInterrupt& m_interrupt;
    std::atomic<bool> m_importing{false};

    /**
     * Whether all blockfiles have been added to the block tree database.
     * Normally true, but set to false when a reindex is requested and the
     * database is wiped. The value is persisted in the database across restarts
     * and will be false until reindexing completes.
     */
    std::atomic_bool m_blockfiles_indexed{true};

    BlockMap m_block_index GUARDED_BY(cs_main);

    /**
     * The height of the base block of an assumeutxo snapshot, if one is in use.
     *
     * This controls how blockfiles are segmented by chainstate type to avoid
     * comingling different height regions of the chain when an assumedvalid chainstate
     * is in use. If heights are drastically different in the same blockfile, pruning
     * suffers.
     *
     * This is set during ActivateSnapshot() or upon LoadBlockIndex() if a snapshot
     * had been previously loaded. After the snapshot is validated, this is unset to
     * restore normal LoadBlockIndex behavior.
     */
    std::optional<int> m_snapshot_height;

    std::vector<CBlockIndex*> GetAllBlockIndices() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
     */
    std::multimap<CBlockIndex*, CBlockIndex*> m_blocks_unlinked;
    void AddUnlinkedBlock(CBlockIndex* block) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    std::unique_ptr<BlockTreeDB> m_block_tree_db GUARDED_BY(::cs_main);

    void WriteBlockIndexDB() EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_index_commit_mutex);
    /**
     * The block index write in two steps: the dirty file info and block index entries are serialised under
     * the lock (and are not dirty afterwards, PrepareBlockIndexBatch), the batch is committed off the lock
     * (CommitBlockIndexBatch). Batches are numbered in preparation order and committed in that order:
     * a batch committed after a later one would overwrite newer entries, and is an assertion failure
     * there.
     *
     * The index never records bytes that a crash could still lose (node/durability_machine.h). A batch
     * prepared while files are unsynced takes their syncs with it, as data: the files and their sizes at
     * that point. A write-out run's effect makes them before it commits the batch (SyncBatchFiles), which
     * needs no lock. A synchronous index write has synced every written file before it prepares its batch
     * (SyncWrittenFiles) and owes none. The commit asserts that none is left. The files count as synced
     * from the preparation on; a batch whose syncs fail is never committed, and no later one is
     * (m_index_batch_lost).
     */
    struct IndexBatch {
        std::unique_ptr<CDBBatch> batch;
        uint64_t generation{0};
        //! The database the batch is for, read under cs_main when the batch was prepared; the commit may
        //! run on the executor without the lock (the pointer never changes after start-up).
        BlockTreeDB* db{nullptr};
        //! The syncs the batch owes, each a file with the size it had when the batch was prepared.
        std::vector<FlatFilePos> block_syncs;
        std::vector<FlatFilePos> undo_syncs;
    };
    IndexBatch PrepareBlockIndexBatch() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Make the syncs the batch owes. False if one fails (reported through flushError()), or if the batch
    //! owes one after a sync failed anywhere since startup (SyncFailed()); the batch then keeps what it
    //! owes and must not be committed. A batch that owes nothing gives true whatever failed before: the
    //! caller asks SyncFailed() first.
    [[nodiscard]] bool SyncBatchFiles(IndexBatch& batch);
    void CommitBlockIndexBatch(const IndexBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(!m_index_commit_mutex);

    /**
     * Sync every block file and undo file written to since its last sync,
     * so that the block index may be written. Returns false if a sync
     * failed, now or at any earlier point since startup (SyncFailed()); the
     * file then stays unsynced.
     */
    [[nodiscard]] bool SyncWrittenFiles() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Whether a sync of a block file or undo file failed since startup. No block index is written
    //! afterwards, by a synchronous write (SyncWrittenFiles()) or by a write-out run
    //! (Chainstate::StartWriteOutRun()): a second sync of the file could report success for pages the
    //! first one lost. Set before the failure is reported. It stops what starts after it is set: a sync
    //! of the same file under way on another thread is not ordered against it.
    bool SyncFailed() const { return m_sync_failed; }

    //! Durability of the block's data and of its undo data, see node::DurabilityState, as the sets of files
    //! written since their last sync say it. A block index batch takes the sets with it when it is prepared
    //! (PrepareBlockIndexBatch()): a file whose sync a prepared batch still owes reads as Synced here.
    //! Tests call these; the node's own guard is the batch's (CommitBlockIndexBatch()).
    DurabilityState DataDurability(const CBlockIndex& block) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    DurabilityState UndoDurability(const CBlockIndex& block) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool LoadBlockIndexDB(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Remove any pruned block & undo files that are still on disk.
     * This could happen on some systems if the file was still being read while unlinked,
     * or if we crash before unlinking.
     */
    void ScanAndUnlinkAlreadyPrunedFiles() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    CBlockIndex* AddToBlockIndex(const CBlockHeader& block, CBlockIndex*& best_header) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Create a new block index entry for a given block hash */
    CBlockIndex* InsertBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Mark one block file as pruned (modify associated database entries)
    void PruneOneBlockFile(int fileNumber) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    CBlockIndex* LookupBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    const CBlockIndex* LookupBlockIndex(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    //! What the index records about a block, as a value (see BlockFacts); nullopt if the hash is unknown.
    std::optional<BlockFacts> LookupBlockFacts(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Get block file info entry for one block file */
    CBlockFileInfo* GetBlockFileInfo(size_t n) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    bool WriteBlockUndo(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * The undo write in three steps, so that it can run on the effects
     * executor: ClaimUndoPos() reserves the space under cs_main (the position
     * returned is that of the storage header), WriteBlockUndoAt() writes the
     * data there with no lock held, and OnUndoWritten() records the stored
     * undo in the block index under cs_main. Callers that need the undo data
     * of a block whose write may still be in flight call SettleUndoWrites()
     * first; ReadBlockUndo() does so itself when the position is not set yet.
     */
    [[nodiscard]] std::optional<FlatFilePos> ClaimUndoPos(BlockValidationState& state, CBlockIndex& block, uint32_t undo_size) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    [[nodiscard]] bool WriteBlockUndoAt(const CBlockUndo& blockundo, const uint256& prev_block_hash, FlatFilePos pos);
    void OnUndoWritten(CBlockIndex& block, const FlatFilePos& data_pos) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /**
     * Where the block's undo file ends when the undo write claimed at `pos` is the file's last (its block file
     * left behind, the block at its last height), else nothing. Decided at the claim, so that the write's
     * effect trims and syncs the file itself (FinishUndoFile()), ahead of any write-out run queued after it:
     * a run's block index batch records the undo data as written (PrepareBlockIndexBatch()). A block file left
     * after the claim has its undo file's sync queued behind the write when it is left (FindNextBlockPos()).
     */
    std::optional<FlatFilePos> UndoFileFinishedBy(const CBlockIndex& block, const FlatFilePos& pos) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Trim and sync an undo file up to `undo_end`; a failure is reported and remembered (SyncFailed()),
    //! not propagated.
    void FinishUndoFile(const FlatFilePos& undo_end);
    //! The undo write claimed for the block has run, written or failed: it no longer counts as pending.
    void UndoWriteSettled(const CBlockIndex& block) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { m_undo_pending.erase(&block); }
    /**
     * Whether the block has every flag of `status_mask`, a BLOCK_HAVE_UNDO whose write is claimed and queued
     * counting as set: the data will be there, in order before anything that may read or prune it, and the
     * flag is set when the write's completion is applied. The availability checks (GetFirstBlock()) use this,
     * so that a just-connected block's undo in flight neither looks pruned nor needs a wait for the executor.
     */
    bool HasStatus(const CBlockIndex& block, uint32_t status_mask) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        if ((block.nStatus & status_mask) == status_mask) return true;
        return (status_mask & BLOCK_HAVE_UNDO) && ((block.nStatus | BLOCK_HAVE_UNDO) & status_mask) == status_mask &&
               m_undo_pending.contains(&block);
    }

    uint64_t m_index_batch_generation GUARDED_BY(::cs_main){0};
    Mutex m_index_commit_mutex;
    uint64_t m_index_batch_committed GUARDED_BY(m_index_commit_mutex){0};

    util::ByteLedger m_own_ledger;
    //! The memory blocks hold in flight; the undo backlog is the executor's account in it.
    util::ByteLedger& m_ledger;

    /**
     * The executor that runs this manager's deferred I/O (undo writes) and
     * carries the notifications that must follow it. Its completions are
     * applied under cs_main: by the worker when it can take the lock without
     * waiting, otherwise by the thread that next settles the queue or applies
     * its completions under the lock (SettleUndoWrites(),
     * ApplyCompletions() and what calls them).
     */
    kernel::EffectsExecutor m_effects{m_ledger[util::ByteAccountId::UndoBacklog], [this] {
        // Never block here: a thread holding cs_main may be waiting for this worker's I/O.
        TRY_LOCK(::cs_main, locked);
        if (locked) m_effects.ApplyCompletions();
    }};
    //! Set on the executor when a block's undo data could not be written (a fatal error is raised with
    //! it). The block is connected in memory by then; no chainstate write is made once this is set, so
    //! that the database never holds a block's coins without the undo data to take them back.
    std::atomic<bool> m_undo_write_failed{false};
    //! Set when a block index batch was taken and not committed: on the executor by a write-out run that
    //! failed before its commit, and by WriteBlockIndexDB() when its own commit throws. No chainstate
    //! write is made once this is set, by any chainstate: the entries the batch took are not dirty any
    //! more, and a later index write would leave them out.
    std::atomic<bool> m_index_batch_lost{false};
    //! Wait for every queued effect's I/O and apply the completions. May be called with or without cs_main.
    //! With blocks in flight this is what preconditions O11 and O7 (validation.h) ask for: the undo data of
    //! every connected block is on disk with its position recorded, and, called with cs_main held, the
    //! signals of the blocks connected so far are handed to the validation interface, in the order they
    //! were queued, before the caller goes on or releases cs_main.
    void SettleUndoWrites();
    //! SettleUndoWrites() only if the block's undo write is queued, for a reader of that block's undo data:
    //! any other block's undo is on disk already or not to be written, and its reader does not wait behind
    //! the executor's queue (a write-out run among it).
    void SettleUndoWriteOf(const CBlockIndex& block) LOCKS_EXCLUDED(::cs_main);
    //! The limit of the ledger's undo backlog account: bytes of undo data queued but not yet written. The
    //! connect loop waits until the account is within it.
    static constexpr size_t MAX_UNDO_BACKLOG_BYTES{64 * 1024 * 1024};

    /** Store block on disk and update block file statistics.
     *
     * @param[in]  block        the block to be stored
     * @param[in]  nHeight      the height of the block
     *
     * @returns in case of success, the position to which the block was written to
     *          in case of an error, an empty FlatFilePos
     */
    FlatFilePos WriteBlock(const CBlock& block, int nHeight) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * A block write in two steps, for callers that want the file write
     * outside cs_main: AllocateBlockPos() claims the space (and does the
     * file bookkeeping) under the lock, WriteBlockAt() writes the block
     * there without it. On success pos is advanced past the storage header
     * to the block's data position, as WriteBlock() returns it. The
     * allocated space is lost if the write fails, which is a fatal error.
     */
    FlatFilePos AllocateBlockPos(const CBlock& block, int nHeight) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    [[nodiscard]] bool WriteBlockAt(const CBlock& block, FlatFilePos& pos);
    /**
     * Between the two steps the caller keeps the block's file from being
     * pruned: the file's heights may all be prunable (a block of the pruned
     * past, stored again), and a file unlinked under the write would leave
     * the index pointing at lost bytes. Unpinned under the hold of cs_main
     * that records the block, or gives it up.
     */
    void PinBlockFile(int file) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    void UnpinBlockFile(int file) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /** Update blockfile info while processing a block during reindex. The block must be available on disk.
     *
     * @param[in]  block        the block being processed
     * @param[in]  nHeight      the height of the block
     * @param[in]  pos          the position of the serialized CBlock on disk
     */
    void UpdateBlockInfo(const CBlock& block, unsigned int nHeight, const FlatFilePos& pos) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Whether running in -prune mode. */
    [[nodiscard]] bool IsPruneMode() const { return m_prune_mode; }

    /** Attempt to stay below this number of bytes of block files. */
    [[nodiscard]] uint64_t GetPruneTarget() const { return m_opts.prune_target; }
    static constexpr auto PRUNE_TARGET_MANUAL{std::numeric_limits<uint64_t>::max()};

    [[nodiscard]] bool LoadingBlocks() const { return m_importing || !m_blockfiles_indexed; }

    /** Calculate the amount of disk space the block & undo files currently use */
    uint64_t CalculateCurrentUsage() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Check if all blocks in the [upper_block, lower_block] range have data available as
    //! defined by the status mask.
    //! The caller is responsible for ensuring that lower_block is an ancestor of upper_block
    //! (part of the same chain).
    bool CheckBlockDataAvailability(const CBlockIndex& upper_block, const CBlockIndex& lower_block, BlockStatus block_status = BLOCK_HAVE_DATA) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * @brief Returns the earliest block with specified `status_mask` flags set after
     * the latest block _not_ having those flags.
     *
     * This function starts from `upper_block`, which must have all
     * `status_mask` flags set, and iterates backwards through its ancestors. It
     * continues as long as each block has all `status_mask` flags set, until
     * reaching the oldest ancestor or `lower_block`.
     *
     * @pre `upper_block` must have all `status_mask` flags set.
     * @pre `lower_block` must be null or an ancestor of `upper_block`
     *
     * @param upper_block The starting block for the search, which must have all
     *                    `status_mask` flags set.
     * @param status_mask Bitmask specifying required status flags.
     * @param lower_block The earliest possible block to return. If null, the
     *                    search can extend to the genesis block.
     *
     * @return A reference to the earliest block between `upper_block`
     *         and `lower_block`, inclusive, such that every block between the
     *         returned block and `upper_block` has `status_mask` flags set.
     */
    const CBlockIndex& GetFirstBlock(
        const CBlockIndex& upper_block LIFETIMEBOUND,
        uint32_t status_mask,
        const CBlockIndex* lower_block LIFETIMEBOUND = nullptr
    ) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

private:
    //! Blocks whose undo write is claimed (ClaimUndoPos()) and not yet settled (UndoWriteSettled()), with the
    //! position in the block's undo file its undo data goes to (as OnUndoWritten() records it).
    std::map<const CBlockIndex*, uint64_t> m_undo_pending GUARDED_BY(::cs_main);

public:

    /** True if any block files have ever been pruned. */
    bool m_have_pruned = false;

    //! Check whether the block associated with this index entry is pruned or not.
    bool IsBlockPruned(const CBlockIndex& block) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Create or update a prune lock identified by its name
    void UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Delete a prune lock identified by its name. Returns true if the lock existed.
    bool DeletePruneLock(const std::string& name) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Open a block file (blk?????.dat) */
    AutoFile OpenBlockFile(const FlatFilePos& pos, bool fReadOnly) const;

    /** Translation to a filesystem path */
    fs::path GetBlockPosFilename(const FlatFilePos& pos) const;

    /**
     *  Actually unlink the specified files
     */
    void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune) const;

    /** Functions for disk access for blocks */
    //! @param[out] bytes_read  If given, the length of the block's serialization read from the file.
    bool ReadBlock(CBlock& block, const FlatFilePos& pos, const std::optional<uint256>& expected_hash, size_t* bytes_read = nullptr) const;
    bool ReadBlock(CBlock& block, const CBlockIndex& index) const;
    ReadRawBlockResult ReadRawBlock(const FlatFilePos& pos, std::optional<std::pair<size_t, size_t>> block_part = std::nullopt) const;

    bool ReadBlockUndo(CBlockUndo& blockundo, const CBlockIndex& index) const;

    void CleanupBlockRevFiles() const;
};

// Calls ActivateBestChain() even if no blocks are imported.
void ImportBlocks(ChainstateManager& chainman, std::span<const fs::path> import_paths);
} // namespace node

#endif // BITCOIN_NODE_BLOCKSTORAGE_H
