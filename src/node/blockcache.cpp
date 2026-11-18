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

size_t BlockCache::Bytes(const CBlock& block)
{
    return sizeof(CBlock) + RecursiveDynamicUsage(block);
}

void BlockCache::Insert(int height, const std::shared_ptr<const CBlock>& block, int tip_height, size_t bytes)
{
    if (m_budget == 0 || height <= tip_height) return;
    // With the budget used up by blocks the tip has not passed, a block above all of them would be the first to
    // go: it is neither measured nor kept (a reindex's files, read far ahead of the tip, pass here). Used up
    // means the block's known size does not fit, or, not measured yet, the last insertion had to drop blocks.
    if (WITH_LOCK(m_mutex, return !m_blocks.empty() && (bytes > 0 ? !m_account.Admits(bytes) : m_full) &&
                                  m_blocks.begin()->first.first > tip_height && height > std::prev(m_blocks.end())->first.first)) {
        return;
    }
    if (bytes == 0) bytes = Bytes(*block);
    if (bytes > m_budget) return;
    LOCK(m_mutex);
    // Blocks the tip has passed will never be taken.
    while (!m_blocks.empty() && m_blocks.begin()->first.first <= tip_height) {
        m_account.Sub(m_blocks.begin()->second.bytes);
        m_blocks.erase(m_blocks.begin());
        m_full = false;
    }
    const auto [it, inserted]{m_blocks.try_emplace({height, block->GetHash()}, Entry{block, bytes})};
    if (!inserted) return;
    m_account.Add(bytes);
    // Over budget: the highest blocks go first, the lowest connect next.
    while (!m_account.Admits()) {
        const auto last{std::prev(m_blocks.end())};
        m_account.Sub(last->second.bytes);
        m_blocks.erase(last);
        m_full = true;
    }
}

void BlockCache::Drop(int height, const uint256& hash)
{
    if (m_budget == 0) return;
    LOCK(m_mutex);
    const auto it{m_blocks.find({height, hash})};
    if (it == m_blocks.end()) return;
    m_account.Sub(it->second.bytes);
    m_blocks.erase(it);
    m_full = false;
}

std::shared_ptr<const CBlock> BlockCache::Take(int height, const uint256& hash, size_t* bytes)
{
    if (m_budget == 0) return nullptr;
    LOCK(m_mutex);
    const auto it{m_blocks.find({height, hash})};
    if (it == m_blocks.end()) return nullptr;
    std::shared_ptr<const CBlock> block{std::move(it->second.block)};
    if (bytes) *bytes = it->second.bytes;
    m_account.Sub(it->second.bytes);
    m_blocks.erase(it);
    m_full = false;
    return block;
}

} // namespace node
