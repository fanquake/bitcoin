// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCKSTORE_MACHINE_H
#define BITCOIN_NODE_BLOCKSTORE_MACHINE_H

#include <chain.h>
#include <util/machine.h>

#include <optional>

namespace node {
/**
 * Block storage as the rest of the node sees it: where one block stands
 * between "its header is known" and "it is part of a chain".
 *
 *   HeaderKnown --DataStored--> Stored ---Connect---> Connected
 *     ^   ^                       ^  <--Disconnect--       ^
 *     |   |                       |   |                    |
 *   Fail  Reconsider            Fail  Reconsider        DataStored
 *     |   |                       |   |                    |
 *     v   |                       v   |              ConnectedPruned
 *   FailedHeader                FailedStored
 *
 * Fail on a failed block leaves it where it is (a descendant of a failed
 * block is marked again when another ancestor fails). Pruning is no event of
 * the table: PruneOneBlockFile() gives up a file's blocks at once, which
 * takes a block's data and nothing else (Stored to HeaderKnown, Connected to
 * ConnectedPruned, FailedStored to FailedHeader).
 *
 * The state is not stored. It is derived from what the block index already
 * records (BLOCK_HAVE_DATA, BLOCK_FAILED_VALID) and from membership in one
 * chain, so a block has one state per chainstate. Every pair not drawn is
 * refused; in particular a block is never failed while it is in the chain
 * (it is disconnected first), a block without data is never connected, and
 * a failed block takes no data.
 *
 * Loading the block index and activating a snapshot establish states
 * wholesale and are not transitions: after either, a block in the chain
 * whose data is absent is ConnectedPruned, whether the data was pruned or
 * (below a snapshot base) never there.
 */
enum class BlockStoreState {
    HeaderKnown,     //!< In the block index, no data, not in the chain.
    Stored,          //!< Data on disk, not in the chain.
    Connected,       //!< In the chain, data on disk.
    ConnectedPruned, //!< In the chain, data absent.
    FailedHeader,    //!< Failed (itself or an ancestor), no data.
    FailedStored,    //!< Failed (itself or an ancestor), data on disk.
};

enum class BlockStoreEvent {
    DataStored, //!< ReceivedBlockTransactions(): the block was written and passed its checks.
    Connect,    //!< ConnectTip(): the block became the tip.
    Disconnect, //!< DisconnectTip(): the block stopped being the tip.
    Fail,       //!< InvalidBlockFound(), InvalidateBlock(), SetBlockFailureFlags().
    Reconsider, //!< ResetBlockFailureFlags().
};

inline constexpr auto BLOCK_STORE_TABLE{util::MakeTransitionTable<BlockStoreState, BlockStoreEvent>({
    {BlockStoreState::HeaderKnown, BlockStoreEvent::DataStored, BlockStoreState::Stored},
    {BlockStoreState::ConnectedPruned, BlockStoreEvent::DataStored, BlockStoreState::Connected},
    {BlockStoreState::Stored, BlockStoreEvent::Connect, BlockStoreState::Connected},
    {BlockStoreState::Connected, BlockStoreEvent::Disconnect, BlockStoreState::Stored},
    {BlockStoreState::HeaderKnown, BlockStoreEvent::Fail, BlockStoreState::FailedHeader},
    {BlockStoreState::Stored, BlockStoreEvent::Fail, BlockStoreState::FailedStored},
    {BlockStoreState::FailedHeader, BlockStoreEvent::Fail, BlockStoreState::FailedHeader},
    {BlockStoreState::FailedStored, BlockStoreEvent::Fail, BlockStoreState::FailedStored},
    {BlockStoreState::FailedHeader, BlockStoreEvent::Reconsider, BlockStoreState::HeaderKnown},
    {BlockStoreState::FailedStored, BlockStoreEvent::Reconsider, BlockStoreState::Stored},
})};

/**
 * The state of `block` relative to `chain`, or nullopt if the block index
 * holds a combination the machine has no state for (failed and in the chain).
 */
inline std::optional<BlockStoreState> BlockStoreStateOf(const CBlockIndex& block, const CChain& chain) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const bool have_data{(block.nStatus & BLOCK_HAVE_DATA) != 0};
    const bool failed{(block.nStatus & BLOCK_FAILED_VALID) != 0};
    if (chain.Contains(block)) {
        if (failed) return std::nullopt;
        return have_data ? BlockStoreState::Connected : BlockStoreState::ConnectedPruned;
    }
    if (failed) return have_data ? BlockStoreState::FailedStored : BlockStoreState::FailedHeader;
    return have_data ? BlockStoreState::Stored : BlockStoreState::HeaderKnown;
}
} // namespace node

#endif // BITCOIN_NODE_BLOCKSTORE_MACHINE_H
