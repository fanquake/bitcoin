// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_SYNC_MODE_H
#define BITCOIN_NODE_SYNC_MODE_H

#include <cstdint>

namespace node {
/**
 * Which of two ways the node processes blocks: what the initial block download
 * latch (ChainstateManager::IsInitialBlockDownload()) selects.
 *
 * The latch falls once, under cs_main: where a block becomes the tip
 * (Chainstate::PublishConnectTip()), where one is disconnected whose parent
 * is dated later (Chainstate::DisconnectTip()), or, outside every operation
 * named below, when the chain is loaded and when an import of blocks ends.
 * An operation that spans more than one hold of cs_main, or holds none, reads
 * it once, where it starts (ChainstateManager::SyncModeNow()), and hands the
 * copy down: it finishes in the mode it started in. The operations: a message
 * off the wire (PeerManagerImpl::ProcessMessage()), a step of an activation
 * (Chainstate::ActivateBestChain()), a pass of block requests to a peer
 * (PeerManagerImpl::SendMessages()).
 *
 * The mode selects whether a step connects a window of blocks, whether a
 * requested block goes to a task of the connect pool, the rule for a peer's
 * block requests, whether the message handler waits for pending blocks,
 * whether a getdata or getblocks activates the chain first, and whether a
 * historical activation gives way. Whatever else reads the latch selects
 * nothing between blocks in flight, and reads it afresh:
 *  - under the hold of cs_main that may change it, where the reading cannot
 *    turn before the hold ends (the signals and notifications of
 *    PublishConnectTip(), FinishConnectStep() and NotifyTipAdvanced(), and
 *    NewPoWValidBlock in AcceptBlock());
 *  - for a transaction, which is one hold of cs_main (the settle before
 *    AcceptToMemoryPool(), precondition O7);
 *  - for what is the same in both modes but for its cost or its log
 *    (compaction, the block bins' release, cache sizes, log levels, the fee
 *    filter, transaction announcements, the headers a compact block asks for).
 */
enum class SyncMode : uint8_t {
    //! Initial block download: blocks are received, connected and written beside each other.
    Download,
    //! Out of it: a block is connected before what follows it is processed.
    Tip,
};

constexpr SyncMode SyncModeOf(bool initial_block_download)
{
    return initial_block_download ? SyncMode::Download : SyncMode::Tip;
}
} // namespace node

#endif // BITCOIN_NODE_SYNC_MODE_H
