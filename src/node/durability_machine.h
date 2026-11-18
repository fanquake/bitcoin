// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_DURABILITY_MACHINE_H
#define BITCOIN_NODE_DURABILITY_MACHINE_H

#include <util/machine.h>

namespace node {
/**
 * Durability of one block's bytes in a block file or an undo file, as far as
 * the block index may rely on them. The machine runs twice per block, once
 * for its data (BLOCK_HAVE_DATA) and once for its undo (BLOCK_HAVE_UNDO).
 *
 *            Write               Sync
 *   Absent ---------> Unsynced ---------> Synced --+ Sync, Record
 *     ^  \               |                  |   ^--+
 *     |   + Record       | Prune            | Prune
 *     |  (stays)         v                  v
 *     +------------------+------------------+
 *
 * The guard is the edge that is missing: Record, the block index database
 * taking the flag, is refused from Unsynced. An index entry that survives a
 * crash must never point at bytes that did not.
 *
 * The state is derived, not stored: the flag in nStatus says whether the
 * bytes exist, and the block manager's sets of files written since their
 * last sync say whether they are safe. The sets are per file, so a block
 * whose own bytes were synced reads as Unsynced again when a later block is
 * written into the same file. That errs on the safe side.
 *
 * A sync that fails in a flush leaves the block Unsynced; one that fails in
 * a write-out run does not, since the run took the file from the sets with
 * its index batch. Either is remembered for the rest of the run: the flush
 * or run that wanted to record the block, and every later one, reports the
 * failure and does not reach the index write. A block then reads Synced
 * although the failed sync may have lost pages, at once after a run and
 * after a later flush of the file otherwise: what keeps the index from it
 * is that memory, not the state.
 *
 * The guarantee is per run. Synced means synced since startup or recorded by
 * an earlier run under the same rule; a block index written by a version
 * without the rule is taken as it is, and so are the block files of a
 * reindex, which records blocks an earlier run wrote without a sync of
 * their files in this one.
 *
 * The node holds the guard at one site, before the index batch is committed
 * (BlockManager::CommitBlockIndexBatch()): a batch that still owes a sync is
 * not committed.
 */
enum class DurabilityState {
    Absent,   //!< No bytes the index could refer to (never written, or pruned).
    Unsynced, //!< Written, the file not synced since.
    Synced,   //!< Written, and the file synced since.
};

/**
 * The order of one write-out of the node's state (FlushStateToDisk()). Each
 * step may rely on what the steps before it made durable, and on nothing
 * else:
 *
 *   Start --SyncFiles--> FilesSynced --WriteIndex--> IndexWritten --WriteCoins--> CoinsWritten
 *                                                      |                              |
 *                                                      +--Unlink--> FilesUnlinked <---+
 *                                                                                  Unlink
 *
 * - the block index is written only after the files it refers to are synced;
 * - the coins are written only after the index holds the block they are at;
 * - pruned files are unlinked last, only after the index stopped referring to them.
 *
 * A write-out run on the executor (Chainstate::StartWriteOutRun()) makes the first three
 * steps in this order off the validation thread, behind one write that is not a step here: the
 * coins that disconnects restored go to the database first. The caller that waited for the run
 * records the three steps once it has ended; a run nobody waits for records none, and this
 * table is not consulted while a run is under way.
 *
 * A step that fails ends the write-out where it stands; there is no edge out
 * of a failure, and the next write-out starts again from Start.
 */
enum class FlushPhase {
    Start,
    FilesSynced,
    IndexWritten,
    FilesUnlinked,
    CoinsWritten,
};

enum class FlushStep {
    SyncFiles,
    WriteIndex,
    Unlink,
    WriteCoins,
};

inline constexpr auto FLUSH_TABLE{util::MakeTransitionTable<FlushPhase, FlushStep>({
    {FlushPhase::Start, FlushStep::SyncFiles, FlushPhase::FilesSynced},
    {FlushPhase::FilesSynced, FlushStep::WriteIndex, FlushPhase::IndexWritten},
    {FlushPhase::IndexWritten, FlushStep::WriteCoins, FlushPhase::CoinsWritten},
    {FlushPhase::IndexWritten, FlushStep::Unlink, FlushPhase::FilesUnlinked},
    {FlushPhase::CoinsWritten, FlushStep::Unlink, FlushPhase::FilesUnlinked},
})};
} // namespace node

#endif // BITCOIN_NODE_DURABILITY_MACHINE_H
