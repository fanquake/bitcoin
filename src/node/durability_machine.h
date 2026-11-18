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
 * A failed sync leaves the block Unsynced for the rest of the run; the flush
 * that wanted to record it, and every later one, reports the failure and
 * does not reach the index write.
 *
 * The guarantee is per run. Synced means synced since startup or recorded by
 * an earlier run under the same rule; a block index written by a version
 * without the rule is taken as it is, and so are the block files of a
 * reindex, which records blocks an earlier run wrote without a sync of
 * their files in this one.
 */
enum class DurabilityState {
    Absent,   //!< No bytes the index could refer to (never written, or pruned).
    Unsynced, //!< Written, the file not synced since.
    Synced,   //!< Written, and the file synced since.
};

enum class DurabilityEvent {
    Write,  //!< A position was claimed and the bytes written.
    Sync,   //!< The file was synced.
    Record, //!< The block index database is written with this entry in it.
    Prune,  //!< The file is given up.
};

inline constexpr auto DURABILITY_TABLE{util::MakeTransitionTable<DurabilityState, DurabilityEvent>({
    {DurabilityState::Absent, DurabilityEvent::Write, DurabilityState::Unsynced},
    {DurabilityState::Unsynced, DurabilityEvent::Sync, DurabilityState::Synced},
    {DurabilityState::Synced, DurabilityEvent::Sync, DurabilityState::Synced},
    {DurabilityState::Absent, DurabilityEvent::Record, DurabilityState::Absent},
    {DurabilityState::Synced, DurabilityEvent::Record, DurabilityState::Synced},
    {DurabilityState::Unsynced, DurabilityEvent::Prune, DurabilityState::Absent},
    {DurabilityState::Synced, DurabilityEvent::Prune, DurabilityState::Absent},
})};

/**
 * The order of one write-out of the node's state (FlushStateToDisk()). Each
 * step may rely on what the steps before it made durable, and on nothing
 * else:
 *
 *   Start --SyncFiles--> FilesSynced --WriteIndex--> IndexWritten --WriteCoins--> CoinsWritten
 *                                                      |                              ^
 *                                                      +--Unlink--> FilesUnlinked ----+
 *                                                                      WriteCoins
 *
 * - the block index is written only after the files it refers to are synced;
 * - pruned files are unlinked only after the index stopped referring to them;
 * - the coins are written only after the index holds the block they are at.
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
    {FlushPhase::IndexWritten, FlushStep::Unlink, FlushPhase::FilesUnlinked},
    {FlushPhase::IndexWritten, FlushStep::WriteCoins, FlushPhase::CoinsWritten},
    {FlushPhase::FilesUnlinked, FlushStep::WriteCoins, FlushPhase::CoinsWritten},
})};
} // namespace node

#endif // BITCOIN_NODE_DURABILITY_MACHINE_H
