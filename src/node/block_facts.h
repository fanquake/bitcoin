// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCK_FACTS_H
#define BITCOIN_NODE_BLOCK_FACTS_H

#include <chain.h>
#include <flatfile.h>
#include <sync.h>

#include <cstdint>

namespace node {
/**
 * What the block index records about one block, copied under cs_main in
 * one read, for a reader that then works without the lock.
 *
 * What a copy promises: the members agree with each other as of the copy.
 * What it does not: that they still hold. In particular a position may stop
 * being readable, because the file can be pruned once the lock is released;
 * a reader either handles the failed read or states why the block cannot
 * be pruned.
 */
struct BlockFacts {
    int height{0};
    uint32_t status{0};
    unsigned int tx_count{0};
    //! Null unless the data is on disk.
    FlatFilePos data_pos;

    bool HaveData() const { return (status & BLOCK_HAVE_DATA) != 0; }
};

inline BlockFacts BlockFactsOf(const CBlockIndex& block) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);
    return BlockFacts{
        .height = block.nHeight,
        .status = block.nStatus,
        .tx_count = block.nTx,
        .data_pos = block.GetBlockPos(),
    };
}
} // namespace node

#endif // BITCOIN_NODE_BLOCK_FACTS_H
