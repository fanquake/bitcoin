// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <node/blockstore_machine.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using node::BLOCK_STORE_TABLE;
using node::BlockStoreEvent;
using node::BlockStoreState;
using node::BlockStoreStateOf;

BOOST_AUTO_TEST_SUITE(blockstore_machine_tests)

BOOST_AUTO_TEST_CASE(block_store_table)
{
    using S = BlockStoreState;
    using E = BlockStoreEvent;
    // The refusals the node relies on, by name.
    BOOST_CHECK(!BLOCK_STORE_TABLE.Next(S::Connected, E::Fail)); // disconnect first
    BOOST_CHECK(!BLOCK_STORE_TABLE.Next(S::ConnectedPruned, E::Fail));
    BOOST_CHECK(!BLOCK_STORE_TABLE.Next(S::HeaderKnown, E::Connect)); // no data, no connect
    BOOST_CHECK(!BLOCK_STORE_TABLE.Next(S::FailedStored, E::Connect)); // a failed block is never connected
    BOOST_CHECK(!BLOCK_STORE_TABLE.Next(S::FailedHeader, E::DataStored)); // a failed block takes no data
    BOOST_CHECK(!BLOCK_STORE_TABLE.Next(S::Stored, E::DataStored)); // a second copy is not stored
    BOOST_CHECK(!BLOCK_STORE_TABLE.Next(S::ConnectedPruned, E::Disconnect)); // nothing to disconnect with
}

BOOST_AUTO_TEST_CASE(block_store_state_is_derived)
{
    LOCK(cs_main);
    CBlockIndex block;
    CChain without;
    CChain with;
    with.SetTip(block);

    BOOST_CHECK(BlockStoreStateOf(block, without) == BlockStoreState::HeaderKnown);
    BOOST_CHECK(BlockStoreStateOf(block, with) == BlockStoreState::ConnectedPruned);
    block.nStatus |= BLOCK_HAVE_DATA;
    BOOST_CHECK(BlockStoreStateOf(block, without) == BlockStoreState::Stored);
    BOOST_CHECK(BlockStoreStateOf(block, with) == BlockStoreState::Connected);
    block.nStatus |= BLOCK_FAILED_VALID;
    BOOST_CHECK(BlockStoreStateOf(block, without) == BlockStoreState::FailedStored);
    // Failed and in the chain: the machine has no such state.
    BOOST_CHECK(!BlockStoreStateOf(block, with));
    block.nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(BlockStoreStateOf(block, without) == BlockStoreState::FailedHeader);
    BOOST_CHECK(!BlockStoreStateOf(block, with));
}

// Drive the transition sites and read the state back after each: the sites check themselves
// (CheckBlockStoreTransition), this pins what they leave behind.
BOOST_FIXTURE_TEST_CASE(block_store_sites, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};
    const auto state_of{[&](const CBlockIndex& block) {
        LOCK(cs_main);
        return BlockStoreStateOf(block, chainstate.m_chain);
    }};

    CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    BOOST_REQUIRE(tip);
    BOOST_CHECK(state_of(*tip) == BlockStoreState::Connected);

    // Disconnect, then Fail.
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, tip));
    BOOST_CHECK(state_of(*tip) == BlockStoreState::FailedStored);

    // Reconsider, as the reconsiderblock RPC does it.
    {
        LOCK(cs_main);
        chainstate.ResetBlockFailureFlags(tip);
        chainman.RecalculateBestHeader();
    }
    BOOST_CHECK(state_of(*tip) == BlockStoreState::Stored);

    // Connect.
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    BOOST_CHECK(state_of(*tip) == BlockStoreState::Connected);

    // Prune.
    {
        LOCK(cs_main);
        blockman.m_have_pruned = true;
        blockman.PruneOneBlockFile(tip->nFile);
    }
    BOOST_CHECK(state_of(*tip) == BlockStoreState::ConnectedPruned);
}

BOOST_AUTO_TEST_SUITE_END()
