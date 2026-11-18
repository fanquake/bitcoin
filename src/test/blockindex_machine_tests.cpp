// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <node/blockstorage.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(blockindex_machine_tests)

// The sequence behind #36149: a block whose data is stored while a parent's is not sits in
// m_blocks_unlinked; pruning its file must take it out again, or it would be offered for
// connection with no data to connect.
BOOST_FIXTURE_TEST_CASE(pruned_block_leaves_the_unlinked_set, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};
    LOCK(cs_main);
    CBlockIndex* tip{chainman.ActiveChain().Tip()};
    BOOST_REQUIRE(tip && tip->pprev);
    BOOST_REQUIRE(tip->nStatus & BLOCK_HAVE_DATA);

    // Pretend the parent's data went missing, which is how the tip would end up in m_blocks_unlinked.
    blockman.AddUnlinkedBlock(tip);
    BOOST_CHECK_EQUAL(blockman.m_blocks_unlinked.count(tip->pprev), 1U);

    const int file{tip->nFile};
    blockman.m_have_pruned = true;
    blockman.PruneOneBlockFile(file);
    BOOST_CHECK(!(tip->nStatus & (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO)));
    BOOST_CHECK_EQUAL(blockman.m_blocks_unlinked.count(tip->pprev), 0U);
    for (const auto& [parent, child] : blockman.m_blocks_unlinked) {
        BOOST_CHECK(child->nStatus & BLOCK_HAVE_DATA); // the invariant CheckBlockIndex enforces
    }
}

BOOST_AUTO_TEST_SUITE_END()
