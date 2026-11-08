// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockcache.h>

#include <core_memusage.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>

#include <iterator>
#include <memory>
#include <utility>

namespace node {

void BlockCache::Insert(int height, const std::shared_ptr<const CBlock>& block, int tip_height)
{
    if (m_budget == 0 || height <= tip_height) return;
    const size_t bytes{sizeof(CBlock) + RecursiveDynamicUsage(*block)};
    if (bytes > m_budget) return;
    LOCK(m_mutex);
    // Blocks the tip has passed will never be taken.
    while (!m_blocks.empty() && m_blocks.begin()->first.first <= tip_height) {
        m_bytes -= m_blocks.begin()->second.bytes;
        m_blocks.erase(m_blocks.begin());
    }
    const auto [it, inserted]{m_blocks.try_emplace({height, block->GetHash()}, Entry{block, bytes})};
    if (!inserted) return;
    m_bytes += bytes;
    // Over budget: the highest blocks go first, the lowest connect next.
    while (m_bytes > m_budget) {
        const auto last{std::prev(m_blocks.end())};
        m_bytes -= last->second.bytes;
        m_blocks.erase(last);
    }
}

std::shared_ptr<const CBlock> BlockCache::Take(int height, const uint256& hash)
{
    if (m_budget == 0) return nullptr;
    LOCK(m_mutex);
    const auto it{m_blocks.find({height, hash})};
    if (it == m_blocks.end()) {
        ++m_misses;
        return nullptr;
    }
    ++m_hits;
    std::shared_ptr<const CBlock> block{std::move(it->second.block)};
    m_bytes -= it->second.bytes;
    m_blocks.erase(it);
    return block;
}

} // namespace node
