// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCKCACHE_H
#define BITCOIN_NODE_BLOCKCACHE_H

#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace node {

/**
 * Accepted blocks kept in memory until they are connected, so that the connect loop takes the object that was
 * received and checked instead of reading the block back from disk and deserialising and hashing it again. During
 * initial block download blocks arrive out of order inside the download window, so the block being connected is
 * rarely the one that just arrived.
 *
 * Bounded by a byte budget: when it is exceeded the highest blocks are dropped first, since the lowest connect
 * next; blocks at or below the active tip are dropped on insertion. An object taken from here still carries
 * CBlock::fChecked, so ConnectBlock() does not run CheckBlock() on it again. A budget of zero disables it.
 */
class BlockCache
{
    mutable Mutex m_mutex;
    const size_t m_budget;
    struct Entry {
        std::shared_ptr<const CBlock> block;
        size_t bytes;
    };
    //! Ordered by height, then hash.
    std::map<std::pair<int, uint256>, Entry> m_blocks GUARDED_BY(m_mutex);
    size_t m_bytes GUARDED_BY(m_mutex){0};
    uint64_t m_hits GUARDED_BY(m_mutex){0};
    uint64_t m_misses GUARDED_BY(m_mutex){0};

public:
    explicit BlockCache(size_t budget) : m_budget{budget} {}

    //! Keep `block`, accepted at `height`, when the budget allows, after dropping blocks at or below `tip_height`.
    void Insert(int height, const std::shared_ptr<const CBlock>& block, int tip_height) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The block `hash` at `height`, removed from the cache; null when it is not here.
    std::shared_ptr<const CBlock> Take(int height, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    size_t Budget() const { return m_budget; }
    size_t Bytes() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { return WITH_LOCK(m_mutex, return m_bytes); }
    size_t Count() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { return WITH_LOCK(m_mutex, return m_blocks.size()); }
    //! Takes that found the block, and takes that did not.
    std::pair<uint64_t, uint64_t> HitsAndMisses() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return {m_hits, m_misses};
    }
};

} // namespace node

#endif // BITCOIN_NODE_BLOCKCACHE_H
