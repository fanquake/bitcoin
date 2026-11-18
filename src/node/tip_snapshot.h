// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_TIP_SNAPSHOT_H
#define BITCOIN_NODE_TIP_SNAPSHOT_H

#include <arith_uint256.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>

namespace node {
/**
 * The current chainstate's tip as one value, for a reader that needs one
 * fact about it and nothing that must agree with other chain state.
 *
 * ChainstateManager publishes a new value as the last step of a cs_main
 * hold that moved a tip, changed which chainstate is the current one or
 * ended initial block download: after the blocks are part of the chain,
 * their coins are in the tip view, their block index entries are complete
 * but for the position of undo data still being written, and the hold's
 * signals are queued or, in initial block download, wait on the block
 * manager's executor behind that write (BlockConnected and the
 * UpdatedBlockTip after it). The notifications of a connected block are
 * sent in its hold, but for ActiveTipChange where blocks are connected in a
 * window: the step sends it once, in a hold after the one that published
 * its last block. InvalidateBlock() publishes with every block it
 * disconnects and sends its two notifications once, after the last and
 * without cs_main. A reader therefore sees the tips cs_main would have
 * shown it, the ones a hold leaves behind, and none of the tips a
 * reorganisation passes through.
 * Loading the chain and activating a snapshot publish before their hold
 * ends, at a point after which it moves no tip. A reader copies the value
 * with ChainstateManager::PublishedTip() and holds no lock while using it;
 * the call waits for cs_main only while such a hold is open.
 *
 * What a copy promises: every member describes the same block, and that
 * block was the tip at the moment of the call. What it does not promise:
 * that the block is still the tip, or that anything else read afterwards
 * (the block index, the mempool, the coins) is in the state it was in at
 * that moment. A reader that needs that takes cs_main and reads the chain.
 */
struct TipSnapshot {
    //! Null while there is no tip (before the genesis block is connected).
    uint256 hash;
    int height{-1};
    arith_uint256 chain_work;
    //! The header's time and bits.
    int64_t time{0};
    uint32_t bits{0};
    //! ChainstateManager::IsInitialBlockDownload() at that moment.
    bool initial_block_download{true};
    //! Counts the tip changes of every chainstate, so it rises with each
    //! publication, by one or more; 0 only before the first.
    uint64_t sequence{0};

    bool HasTip() const { return height >= 0; }
};

/**
 * Where the published value lives. The mutex is held for a copy and for
 * nothing else, so a reader never waits for work done under another lock.
 */
class PublishedTip
{
    mutable Mutex m_mutex;
    TipSnapshot m_value GUARDED_BY(m_mutex);

public:
    TipSnapshot Get() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_value;
    }

    //! Replace the value. The caller numbers it (ChainstateManager::PublishTip()).
    void Publish(const TipSnapshot& next) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_value = next;
    }
};
} // namespace node

#endif // BITCOIN_NODE_TIP_SNAPSHOT_H
