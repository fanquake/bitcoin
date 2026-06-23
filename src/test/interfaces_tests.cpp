// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <script/solver.h>
#include <validation.h>

#include <test/util/framework.h>

using interfaces::FoundBlock;

TEST_SUITE_BEGIN(interfaces_tests)

FIXTURE_TEST_CASE(findBlock, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();

    uint256 hash;
    CHECK(chain->findBlock(active[10]->GetBlockHash(), FoundBlock().hash(hash)));
    CHECK(hash == active[10]->GetBlockHash());

    int height = -1;
    CHECK(chain->findBlock(active[20]->GetBlockHash(), FoundBlock().height(height)));
    CHECK(height == active[20]->nHeight);

    CBlock data;
    CHECK(chain->findBlock(active[30]->GetBlockHash(), FoundBlock().data(data)));
    CHECK(data.GetHash() == active[30]->GetBlockHash());

    int64_t time = -1;
    CHECK(chain->findBlock(active[40]->GetBlockHash(), FoundBlock().time(time)));
    CHECK(time == active[40]->GetBlockTime());

    int64_t max_time = -1;
    CHECK(chain->findBlock(active[50]->GetBlockHash(), FoundBlock().maxTime(max_time)));
    CHECK(max_time == active[50]->GetBlockTimeMax());

    int64_t mtp_time = -1;
    CHECK(chain->findBlock(active[60]->GetBlockHash(), FoundBlock().mtpTime(mtp_time)));
    CHECK(mtp_time == active[60]->GetMedianTimePast());

    bool cur_active{false}, next_active{false};
    uint256 next_hash;
    CHECK(active.Height() == 100);
    CHECK(chain->findBlock(active[99]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active).hash(next_hash))));
    CHECK(cur_active);
    CHECK(next_active);
    CHECK(next_hash == active[100]->GetBlockHash());
    cur_active = next_active = false;
    CHECK(chain->findBlock(active[100]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active))));
    CHECK(cur_active);
    CHECK(!next_active);

    CHECK(!chain->findBlock({}, FoundBlock()));
}

FIXTURE_TEST_CASE(findFirstBlockWithTimeAndHeight, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    uint256 hash;
    int height;
    CHECK(chain->findFirstBlockWithTimeAndHeight(/* min_time= */ 0, /* min_height= */ 5, FoundBlock().hash(hash).height(height)));
    CHECK(hash == active[5]->GetBlockHash());
    CHECK(height == 5);
    CHECK(!chain->findFirstBlockWithTimeAndHeight(/* min_time= */ active.Tip()->GetBlockTimeMax() + 1, /* min_height= */ 0));
}

FIXTURE_TEST_CASE(findAncestorByHeight, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    uint256 hash;
    CHECK(chain->findAncestorByHeight(active[20]->GetBlockHash(), 10, FoundBlock().hash(hash)));
    CHECK(hash == active[10]->GetBlockHash());
    CHECK(!chain->findAncestorByHeight(active[10]->GetBlockHash(), 20));
}

FIXTURE_TEST_CASE(findAncestorByHash, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    int height = -1;
    CHECK(chain->findAncestorByHash(active[20]->GetBlockHash(), active[10]->GetBlockHash(), FoundBlock().height(height)));
    CHECK(height == 10);
    CHECK(!chain->findAncestorByHash(active[10]->GetBlockHash(), active[20]->GetBlockHash()));
}

FIXTURE_TEST_CASE(findCommonAncestor, TestChain100Setup)
{
    auto& chain = m_node.chain;
    const CChain& active{*WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return &Assert(m_node.chainman)->ActiveChain())};
    auto* orig_tip = active.Tip();
    for (int i = 0; i < 10; ++i) {
        BlockValidationState state;
        m_node.chainman->ActiveChainstate().InvalidateBlock(state, active.Tip());
    }
    CHECK(active.Height() == orig_tip->nHeight - 10);
    coinbaseKey.MakeNewKey(true);
    for (int i = 0; i < 20; ++i) {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }
    CHECK(active.Height() == orig_tip->nHeight + 10);
    uint256 fork_hash;
    int fork_height;
    int orig_height;
    CHECK(chain->findCommonAncestor(orig_tip->GetBlockHash(), active.Tip()->GetBlockHash(), FoundBlock().height(fork_height).hash(fork_hash), FoundBlock().height(orig_height)));
    CHECK(orig_height == orig_tip->nHeight);
    CHECK(fork_height == orig_tip->nHeight - 10);
    CHECK(fork_hash == active[fork_height]->GetBlockHash());

    uint256 active_hash, orig_hash;
    CHECK(!chain->findCommonAncestor(active.Tip()->GetBlockHash(), {}, {}, FoundBlock().hash(active_hash), {}));
    CHECK(!chain->findCommonAncestor({}, orig_tip->GetBlockHash(), {}, {}, FoundBlock().hash(orig_hash)));
    CHECK(active_hash == active.Tip()->GetBlockHash());
    CHECK(orig_hash == orig_tip->GetBlockHash());
}

FIXTURE_TEST_CASE(hasBlocks, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();

    // Test ranges
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[5]->nStatus &= ~BLOCK_HAVE_DATA;
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[95]->nStatus &= ~BLOCK_HAVE_DATA;
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[50]->nStatus &= ~BLOCK_HAVE_DATA;
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));

    // Test edge cases
    CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 6, 49));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 5, 49));
    CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 6, 50));
}

TEST_SUITE_END()
