// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/validation.h>
#include <kernel/mempool_removal_reason.h>
#include <node/block_facts.h>
#include <node/mempool_persist.h>
#include <node/tip_snapshot.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <txmempool.h>
#include <util/fs.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_FIXTURE_TEST_SUITE(tip_snapshot_tests, TestChain100Setup)

//! The published value and the chain agree whenever cs_main is free.
static void CheckMatchesChain(ChainstateManager& chainman)
{
    // Nothing is left unpublished, so the value read is the published one.
    BOOST_CHECK(!chainman.TipUnpublished());
    LOCK(::cs_main);
    const node::TipSnapshot published{chainman.PublishedTip()};
    const CBlockIndex* tip{chainman.ActiveTip()};
    BOOST_REQUIRE(tip);
    BOOST_CHECK(published.HasTip());
    BOOST_CHECK(published.hash == tip->GetBlockHash());
    BOOST_CHECK_EQUAL(published.height, tip->nHeight);
    BOOST_CHECK(published.chain_work == tip->nChainWork);
    BOOST_CHECK_EQUAL(published.time, tip->GetBlockTime());
    BOOST_CHECK_EQUAL(published.bits, tip->nBits);
    BOOST_CHECK_EQUAL(published.initial_block_download, chainman.IsInitialBlockDownload());
}

BOOST_AUTO_TEST_CASE(published_tip_follows_the_chain)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    const CScript script{CScript{} << OP_TRUE};

    CheckMatchesChain(chainman);
    const node::TipSnapshot at_100{chainman.PublishedTip()};
    BOOST_CHECK_EQUAL(at_100.height, 100);
    BOOST_CHECK_GT(at_100.sequence, 0U);

    // Connecting a block moves the tip once and publishes once.
    CreateAndProcessBlock({}, script);
    CheckMatchesChain(chainman);
    const node::TipSnapshot at_101{chainman.PublishedTip()};
    BOOST_CHECK_EQUAL(at_101.height, 101);
    BOOST_CHECK_EQUAL(at_101.sequence, at_100.sequence + 1);

    // A copy taken earlier is unchanged: it is a value, not a view.
    BOOST_CHECK_EQUAL(at_100.height, 100);

    // Disconnecting publishes too, and the sequence number never repeats
    // even though the tip does.
    BlockValidationState state;
    CBlockIndex* tip{WITH_LOCK(::cs_main, return chainman.ActiveTip())};
    BOOST_REQUIRE(chainman.ActiveChainstate().InvalidateBlock(state, tip));
    CheckMatchesChain(chainman);
    const node::TipSnapshot back_at_100{chainman.PublishedTip()};
    BOOST_CHECK(back_at_100.hash == at_100.hash);
    BOOST_CHECK_GT(back_at_100.sequence, at_101.sequence);
}

BOOST_AUTO_TEST_CASE(published_tip_follows_the_ibd_latch)
{
    auto& chainman{static_cast<TestChainstateManager&>(*Assert(m_node.chainman))};
    if (!chainman.IsInitialBlockDownload()) chainman.ResetIbd();
    const node::TipSnapshot during{chainman.PublishedTip()};
    BOOST_CHECK(during.initial_block_download);
    CheckMatchesChain(chainman);

    // The end of initial block download moves no tip and is published
    // under a number of its own.
    chainman.JumpOutOfIbd();
    const node::TipSnapshot after{chainman.PublishedTip()};
    BOOST_CHECK(!after.initial_block_download);
    BOOST_CHECK(after.hash == during.hash);
    BOOST_CHECK_EQUAL(after.sequence, during.sequence + 1);
    CheckMatchesChain(chainman);
}

BOOST_AUTO_TEST_CASE(published_tip_follows_a_block_that_ends_ibd)
{
    auto& chainman{static_cast<TestChainstateManager&>(*Assert(m_node.chainman))};
    if (!chainman.IsInitialBlockDownload()) chainman.ResetIbd();
    const node::TipSnapshot during{chainman.PublishedTip()};
    BOOST_REQUIRE(during.initial_block_download);

    // A recent block ends initial block download in the hold that connects
    // it: the published value has the new tip and the new answer.
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    const node::TipSnapshot after{chainman.PublishedTip()};
    BOOST_CHECK(!after.initial_block_download);
    BOOST_CHECK_EQUAL(after.height, during.height + 1);
    BOOST_CHECK_GT(after.sequence, during.sequence);
    CheckMatchesChain(chainman);
}

//! Reads the tip from inside the cs_main hold that moved it.
struct InsideTheHold final : public CValidationInterface {
    ChainstateManager& m_chainman;
    std::vector<bool> m_unpublished;
    std::vector<int> m_heights;
    explicit InsideTheHold(ChainstateManager& chainman) : m_chainman{chainman} {}
    void ActiveTipChange(const CBlockIndex&, bool) override
    {
        m_unpublished.push_back(m_chainman.TipUnpublished());
        m_heights.push_back(m_chainman.PublishedTip().height);
    }
};

BOOST_AUTO_TEST_CASE(tip_is_published_at_the_end_of_the_hold)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    ValidationSignals& signals{*Assert(m_node.validation_signals)};
    InsideTheHold inside{chainman};
    signals.RegisterValidationInterface(&inside);

    const node::TipSnapshot before{chainman.PublishedTip()};
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);

    // While the hold was open nothing was published, and a reader holding
    // cs_main saw the tip of that moment, as it would reading the chain.
    BOOST_REQUIRE_EQUAL(inside.m_unpublished.size(), 1U);
    BOOST_CHECK(inside.m_unpublished[0]);
    BOOST_CHECK_EQUAL(inside.m_heights[0], before.height + 1);
    // The hold published before it released cs_main.
    CheckMatchesChain(chainman);
    BOOST_CHECK_EQUAL(chainman.PublishedTip().sequence, before.sequence + 1);

    // A tip moved outside the holds that publish leaves readers on
    // cs_main until the next publication.
    {
        LOCK(::cs_main);
        chainman.MarkTipChanged();
        BOOST_CHECK(chainman.TipUnpublished());
        BOOST_CHECK(chainman.PublishedTip().hash == chainman.ActiveTip()->GetBlockHash());
        chainman.PublishTip();
        BOOST_CHECK(!chainman.TipUnpublished());
    }
    BOOST_CHECK_EQUAL(chainman.PublishedTip().sequence, before.sequence + 2);

    signals.UnregisterValidationInterface(&inside);
}

BOOST_AUTO_TEST_CASE(block_facts_are_one_copy)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman.ActiveTip()};
    BOOST_REQUIRE(tip);

    BOOST_CHECK(!chainman.m_blockman.LookupBlockFacts(uint256::ONE));

    const auto facts{chainman.m_blockman.LookupBlockFacts(tip->GetBlockHash())};
    BOOST_REQUIRE(facts);
    BOOST_CHECK_EQUAL(facts->height, tip->nHeight);
    BOOST_CHECK_EQUAL(facts->status, tip->nStatus);
    BOOST_CHECK_EQUAL(facts->tx_count, tip->nTx);
    BOOST_CHECK(facts->HaveData());
    BOOST_CHECK(facts->data_pos == tip->GetBlockPos());
}

//! Records the order of a block's and a transaction's announcement.
struct AnnouncementOrder final : public CValidationInterface {
    std::vector<char> m_order;
    void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>&, const CBlockIndex*) override
    {
        m_order.push_back('b');
    }
    void TransactionAddedToMempool(const NewMempoolTransactionInfo&, uint64_t) override
    {
        m_order.push_back('t');
    }
};

//! In initial block download a block's BlockConnected waits for the block's undo write. A transaction
//! accepted after the block is still announced after it.
BOOST_AUTO_TEST_CASE(a_transaction_is_announced_after_the_block_in_ibd)
{
    auto& chainman{static_cast<TestChainstateManager&>(*Assert(m_node.chainman))};
    ValidationSignals& signals{*Assert(m_node.validation_signals)};

    // A block that is two days old when it arrives leaves the node in initial block download.
    const CBlock block{CreateBlock({}, CScript{} << OP_TRUE)};
    m_clock += std::chrono::hours{48};
    chainman.ResetIbd();
    signals.SyncWithValidationInterfaceQueue();

    AnnouncementOrder order;
    signals.RegisterValidationInterface(&order);

    bool new_block{false};
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block));
    BOOST_REQUIRE(new_block);
    BOOST_REQUIRE(chainman.IsInitialBlockDownload());
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.ActiveTip()->GetBlockHash()) == block.GetHash());

    const CMutableTransaction tx{CreateValidMempoolTransaction(m_coinbase_txns[0], /*input_vout=*/0, /*input_height=*/1, coinbaseKey,
                                                               CScript{} << OP_0 << std::vector<uint8_t>(20, 1), CAmount{1 * COIN}, /*submit=*/false)};
    const MempoolAcceptResult result{WITH_LOCK(::cs_main, return chainman.ProcessTransaction(MakeTransactionRef(tx)))};
    BOOST_REQUIRE(result.m_result_type == MempoolAcceptResult::ResultType::VALID);
    signals.SyncWithValidationInterfaceQueue();
    signals.UnregisterValidationInterface(&order);

    const std::vector<char> expected{'b', 't'};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.m_order.begin(), order.m_order.end(), expected.begin(), expected.end());
}

//! The same for a transaction the node loads from its mempool file: in initial block download
//! LoadMempool() hands the signals of the blocks connected so far over before each transaction.
//!
//! Nothing here can see whether the block's signal still waited when the file was loaded, and a round in
//! which it had left already passes whatever LoadMempool() does. So the round is run several times, each
//! with a block of its own: a LoadMempool() that does not settle the signals has to be missed by all of
//! them.
BOOST_AUTO_TEST_CASE(a_loaded_transaction_is_announced_after_the_block_in_ibd)
{
    auto& chainman{static_cast<TestChainstateManager&>(*Assert(m_node.chainman))};
    ValidationSignals& signals{*Assert(m_node.validation_signals)};
    CTxMemPool& pool{*Assert(m_node.mempool)};

    // A mempool file with one transaction, which the pool then no longer holds.
    const CMutableTransaction tx{CreateValidMempoolTransaction(m_coinbase_txns[0], /*input_vout=*/0, /*input_height=*/1, coinbaseKey,
                                                               CScript{} << OP_0 << std::vector<uint8_t>(20, 1), CAmount{1 * COIN}, /*submit=*/true)};
    const fs::path path{m_args.GetDataDirNet() / "mempool_one_transaction.dat"};
    BOOST_REQUIRE(node::DumpMempool(pool, path));
    const auto remove_transaction{[&] {
        LOCK2(::cs_main, pool.cs);
        pool.removeRecursive(CTransaction{tx}, MemPoolRemovalReason::REPLACED);
        BOOST_REQUIRE_EQUAL(pool.size(), 0U);
    }};
    remove_transaction();

    for (int round{0}; round < 3; ++round) {
        // A block that is two days old when it arrives leaves the node in initial block download.
        const CBlock block{CreateBlock({}, CScript{} << OP_TRUE)};
        m_clock += std::chrono::hours{48};
        chainman.ResetIbd();
        signals.SyncWithValidationInterfaceQueue();

        AnnouncementOrder order;
        signals.RegisterValidationInterface(&order);

        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block));
        BOOST_REQUIRE(new_block);
        BOOST_REQUIRE(chainman.IsInitialBlockDownload());

        BOOST_REQUIRE(node::LoadMempool(pool, path, chainman.ActiveChainstate(), {}));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 1U);
        signals.SyncWithValidationInterfaceQueue();
        signals.UnregisterValidationInterface(&order);

        const std::vector<char> expected{'b', 't'};
        BOOST_CHECK_EQUAL_COLLECTIONS(order.m_order.begin(), order.m_order.end(), expected.begin(), expected.end());

        // The next round loads the file again.
        remove_transaction();
    }
}

BOOST_AUTO_TEST_SUITE_END()
