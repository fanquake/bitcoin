// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for the pure block validation core. Deliberately no TestingSetup:
// there is no ChainstateManager, no block index map, no disk and no cs_main
// here. Everything the functions need is built by hand and passed in.

#include <chain.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <undo.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <utility>

BOOST_AUTO_TEST_SUITE(validation_core_tests)

namespace {

struct Chain {
    std::unique_ptr<const CChainParams> params{CChainParams::RegTest()};
    uint256 genesis_hash{params->GenesisBlock().GetHash()};
    CBlockIndex genesis_index{params->GenesisBlock()};
    //! A stand-in for block 1, the parent of the blocks under test. Only its
    //! index entry exists; the test blocks spend a coin "created" in it.
    uint256 prev_hash{2};
    CBlockIndex prev_index;
    ValidationCache cache{1 << 20, 1 << 20};

    Chain()
    {
        genesis_index.nHeight = 0;
        genesis_index.phashBlock = &genesis_hash;
        prev_index.nHeight = 1;
        prev_index.pprev = &genesis_index;
        prev_index.phashBlock = &prev_hash;
        prev_index.nTime = params->GenesisBlock().nTime + 1;
    }

    const Consensus::Params& Consensus() const { return params->GetConsensus(); }

    BlockValidationContext Context(const uint256& block_hash, bool check_scripts = true) const
    {
        // Regtest has every buried deployment active from the start.
        const DeploymentFlags deployments{.heightincb = true, .dersig = true, .cltv = true, .csv = true, .segwit = true};
        return BlockValidationContext{
            .params = Consensus(),
            .now = NodeClock::now(),
            .deployments = deployments,
            .script_flags = GetBlockScriptFlags(block_hash, deployments, Consensus()),
            .check_scripts = check_scripts,
            .defer_missing_inputs = false,
        };
    }

    //! A block at height 2 containing a coinbase and `spend`, mined to regtest difficulty.
    CBlock MakeBlock(const CTransactionRef& spend, CAmount coinbase_value) const
    {
        CMutableTransaction coinbase;
        coinbase.vin.emplace_back(COutPoint{}, CScript() << 1 << OP_0);
        coinbase.vout.emplace_back(coinbase_value, CScript() << OP_TRUE);

        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev_hash;
        block.nTime = params->GenesisBlock().nTime + 2;
        block.nBits = params->GenesisBlock().nBits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.vtx.push_back(spend);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, Consensus())) ++block.nNonce;
        return block;
    }
};

//! The coin the test block spends, seeded into a fresh view below the block.
const COutPoint SEEDED_OUT{Txid::FromUint256(uint256::ONE), 0};

std::unique_ptr<CCoinsViewCache> SeededView(const Chain& chain, const CScript& script_pub_key)
{
    auto view{std::make_unique<CCoinsViewCache>(&CoinsViewEmpty::Get())};
    view->SetBestBlock(chain.prev_hash);
    view->AddCoin(SEEDED_OUT, Coin{CTxOut{COIN, script_pub_key}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
    return view;
}

CTransactionRef Spend(CAmount value)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(SEEDED_OUT);
    tx.vout.emplace_back(value, CScript() << OP_TRUE);
    return MakeTransactionRef(std::move(tx));
}

} // namespace

BOOST_AUTO_TEST_CASE(connect_then_disconnect_roundtrip)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CBlock block{chain.MakeBlock(Spend(COIN - fee), GetBlockSubsidy(2, chain.Consensus()) + fee)};
    const uint256 hash{block.GetHash()};
    CBlockIndex index{block};
    index.pprev = &chain.prev_index;
    index.nHeight = 2;
    index.phashBlock = &hash;

    auto view{SeededView(chain, CScript() << OP_TRUE)};
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_REQUIRE_MESSAGE(ConnectBlock(block, chain.Context(hash), &index, *view, /*check_queue=*/nullptr, chain.cache, /*just_check=*/false, state, result), state.ToString());
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(!result.failed_in_check_block);
    BOOST_CHECK_EQUAL(result.num_inputs, 2);
    BOOST_CHECK_EQUAL(result.undo.vtxundo.size(), 1U);
    BOOST_REQUIRE_EQUAL(result.undo.vtxundo[0].vprevout.size(), 1U);
    BOOST_CHECK_EQUAL(result.undo.vtxundo[0].vprevout[0].out.nValue, COIN);
    BOOST_CHECK(view->GetBestBlock() == hash);
    BOOST_CHECK(!view->HaveCoin(SEEDED_OUT));
    BOOST_CHECK(view->HaveCoin(COutPoint{block.vtx[0]->GetHash(), 0}));
    BOOST_CHECK(view->HaveCoin(COutPoint{block.vtx[1]->GetHash(), 0}));

    // The returned undo data is all that is needed to roll the view back.
    BOOST_CHECK_EQUAL(DisconnectBlock(block, std::move(result.undo), &index, *view), DISCONNECT_OK);
    BOOST_CHECK(view->GetBestBlock() == chain.prev_hash);
    BOOST_CHECK(view->HaveCoin(SEEDED_OUT));
    BOOST_CHECK_EQUAL(view->AccessCoin(SEEDED_OUT).out.nValue, COIN);
    BOOST_CHECK(!view->HaveCoin(COutPoint{block.vtx[0]->GetHash(), 0}));
    BOOST_CHECK(!view->HaveCoin(COutPoint{block.vtx[1]->GetHash(), 0}));
}

BOOST_AUTO_TEST_CASE(coinbase_overpay_rejected)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CBlock block{chain.MakeBlock(Spend(COIN - fee), GetBlockSubsidy(2, chain.Consensus()) + fee + 1)};
    const uint256 hash{block.GetHash()};
    CBlockIndex index{block};
    index.pprev = &chain.prev_index;
    index.nHeight = 2;
    index.phashBlock = &hash;

    auto view{SeededView(chain, CScript() << OP_TRUE)};
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_CHECK(!ConnectBlock(block, chain.Context(hash), &index, *view, nullptr, chain.cache, false, state, result));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amount");
    BOOST_CHECK(!result.failed_in_check_block);
}

BOOST_AUTO_TEST_CASE(script_checks_follow_the_context)
{
    Chain chain;
    const CBlock block{chain.MakeBlock(Spend(COIN), GetBlockSubsidy(2, chain.Consensus()))};
    const uint256 hash{block.GetHash()};
    CBlockIndex index{block};
    index.pprev = &chain.prev_index;
    index.nHeight = 2;
    index.phashBlock = &hash;

    // The seeded coin is unspendable, so the spend fails script verification...
    {
        auto view{SeededView(chain, CScript() << OP_FALSE)};
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!ConnectBlock(block, chain.Context(hash, /*check_scripts=*/true), &index, *view, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason().substr(0, 32), "block-script-verify-flag-failed ");
    }
    // ...unless the context says scripts are not to be checked, which is
    // the whole of what -assumevalid means to the core.
    {
        auto view{SeededView(chain, CScript() << OP_FALSE)};
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK_MESSAGE(ConnectBlock(block, chain.Context(hash, /*check_scripts=*/false), &index, *view, nullptr, chain.cache, false, state, result), state.ToString());
        BOOST_CHECK(view->GetBestBlock() == hash);
    }
}

BOOST_AUTO_TEST_CASE(just_check_leaves_best_block)
{
    Chain chain;
    const CBlock block{chain.MakeBlock(Spend(COIN), GetBlockSubsidy(2, chain.Consensus()))};
    const uint256 hash{block.GetHash()};
    CBlockIndex index{block};
    index.pprev = &chain.prev_index;
    index.nHeight = 2;
    index.phashBlock = &hash;

    auto view{SeededView(chain, CScript() << OP_TRUE)};
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_CHECK_MESSAGE(ConnectBlock(block, chain.Context(hash), &index, *view, nullptr, chain.cache, /*just_check=*/true, state, result), state.ToString());
    BOOST_CHECK(view->GetBestBlock() == chain.prev_hash);
    BOOST_CHECK(!view->HaveCoin(SEEDED_OUT));
}

//! Deferral waits only for inputs other blocks produce. A transaction spending an output of a later
//! transaction of the same block is invalid in order, and invalid with deferral on too, at once.
BOOST_AUTO_TEST_CASE(deferral_rejects_an_input_from_later_in_the_block)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CTransactionRef later{Spend(COIN - fee)}; // spends the seeded coin
    CMutableTransaction earlier_mut;
    earlier_mut.vin.emplace_back(COutPoint{later->GetHash(), 0});
    earlier_mut.vout.emplace_back(COIN - 2 * fee, CScript() << OP_TRUE);
    const CTransactionRef earlier{MakeTransactionRef(std::move(earlier_mut))};
    CBlock block{chain.MakeBlock(later, GetBlockSubsidy(2, chain.Consensus()) + 2 * fee)};
    block.vtx = {block.vtx[0], earlier, later}; // the spender before the transaction it spends
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, chain.Consensus())) ++block.nNonce;
    const uint256 hash{block.GetHash()};
    CBlockIndex index{block};
    index.pprev = &chain.prev_index;
    index.nHeight = 2;
    index.phashBlock = &hash;

    for (const bool defer : {false, true}) {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationContext ctx{chain.Context(hash)};
        ctx.defer_missing_inputs = defer;
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!ConnectBlock(block, ctx, &index, *view, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
        BOOST_CHECK(result.deferred_txs.empty());
    }
}

//! A block whose input is created by a lower block that is not in the view yet is applied with the
//! transaction deferred, and completed once the lower block is in; the result is what connecting in order
//! gives. Without deferral the same block is simply invalid.
BOOST_AUTO_TEST_CASE(deferred_transaction_completes_when_input_arrives)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    // Block A at height 2 spends the seeded coin into OP_TRUE; block B at height 3 spends A's output.
    const CTransactionRef spend_a{Spend(COIN - fee)};
    const CBlock block_a{chain.MakeBlock(spend_a, GetBlockSubsidy(2, chain.Consensus()) + fee)};
    const uint256 hash_a{block_a.GetHash()};
    CBlockIndex index_a{block_a};
    index_a.pprev = &chain.prev_index;
    index_a.nHeight = 2;
    index_a.phashBlock = &hash_a;

    CMutableTransaction spend_b_mut;
    spend_b_mut.vin.emplace_back(COutPoint{spend_a->GetHash(), 0});
    spend_b_mut.vout.emplace_back(COIN - 2 * fee, CScript() << OP_TRUE);
    const CTransactionRef spend_b{MakeTransactionRef(std::move(spend_b_mut))};
    CMutableTransaction coinbase_b;
    coinbase_b.vin.emplace_back(COutPoint{}, CScript() << 3 << OP_0);
    coinbase_b.vout.emplace_back(GetBlockSubsidy(3, chain.Consensus()) + fee, CScript() << OP_TRUE);
    CBlock block_b;
    block_b.nVersion = 4;
    block_b.hashPrevBlock = hash_a;
    block_b.nTime = block_a.nTime + 1;
    block_b.nBits = block_a.nBits;
    block_b.vtx.push_back(MakeTransactionRef(std::move(coinbase_b)));
    block_b.vtx.push_back(spend_b);
    block_b.hashMerkleRoot = BlockMerkleRoot(block_b);
    while (!CheckProofOfWork(block_b.GetHash(), block_b.nBits, chain.Consensus())) ++block_b.nNonce;
    const uint256 hash_b{block_b.GetHash()};
    CBlockIndex index_b{block_b};
    index_b.pprev = &index_a;
    index_b.nHeight = 3;
    index_b.phashBlock = &hash_b;

    // In order, for reference.
    auto in_order{SeededView(chain, CScript() << OP_TRUE)};
    {
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_REQUIRE(ConnectBlock(block_a, chain.Context(hash_a), &index_a, *in_order, nullptr, chain.cache, false, state, result));
        BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, chain.Context(hash_b), &index_b, *in_order, nullptr, chain.cache, false, state, result), state.ToString());
    }

    // B first, with deferral allowed: its transaction is deferred, its outputs exist, the coinbase check waits.
    auto view{SeededView(chain, CScript() << OP_TRUE)};
    view->SetBestBlock(hash_a); // B's parent is A; the view claims to be at A though A's changes are missing
    BlockValidationContext ctx_b{chain.Context(hash_b)};
    ctx_b.defer_missing_inputs = true;
    BlockValidationState state_b;
    ConnectBlockResult result_b;
    BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, ctx_b, &index_b, *view, nullptr, chain.cache, false, state_b, result_b), state_b.ToString());
    BOOST_REQUIRE_EQUAL(result_b.deferred_txs.size(), 1U);
    BOOST_CHECK_EQUAL(result_b.deferred_txs[0], 1U);
    BOOST_CHECK(result_b.coinbase_check_deferred);
    BOOST_CHECK(view->HaveCoin(COutPoint{spend_b->GetHash(), 0}));
    BOOST_CHECK(result_b.undo.vtxundo.size() == 1 && result_b.undo.vtxundo[0].vprevout.empty());

    // Now A's changes arrive (as the lower block's apply would bring them), and B is finished.
    view->AddCoin(COutPoint{spend_a->GetHash(), 0}, Coin{spend_a->vout[0], 2, false}, false);
    view->SpendCoin(SEEDED_OUT);
    BOOST_REQUIRE_MESSAGE(FinishDeferredTransactions(block_b, ctx_b, &index_b, *view, nullptr, chain.cache, state_b, result_b), state_b.ToString());
    BOOST_CHECK(result_b.deferred_txs.empty());
    BOOST_CHECK(!result_b.coinbase_check_deferred);
    BOOST_CHECK_EQUAL(result_b.fees, fee);
    BOOST_REQUIRE_EQUAL(result_b.undo.vtxundo[0].vprevout.size(), 1U);
    BOOST_CHECK_EQUAL(result_b.undo.vtxundo[0].vprevout[0].out.nValue, COIN - fee);
    BOOST_CHECK(!view->HaveCoin(COutPoint{spend_a->GetHash(), 0}));
    BOOST_CHECK(view->HaveCoin(COutPoint{spend_b->GetHash(), 0}));
    BOOST_CHECK(view->HaveCoin(COutPoint{block_b.vtx[0]->GetHash(), 0}));
    BOOST_CHECK_EQUAL(view->HaveCoin(SEEDED_OUT), in_order->HaveCoin(SEEDED_OUT));

    // The deferred transaction's input never arriving is a consensus failure of B, attributed as such.
    {
        auto lonely{SeededView(chain, CScript() << OP_TRUE)};
        lonely->SetBestBlock(hash_a);
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_REQUIRE(ConnectBlock(block_b, ctx_b, &index_b, *lonely, nullptr, chain.cache, false, state, result));
        BOOST_CHECK(!FinishDeferredTransactions(block_b, ctx_b, &index_b, *lonely, nullptr, chain.cache, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
    // Without deferral the block is invalid at once, as today.
    {
        auto strict{SeededView(chain, CScript() << OP_TRUE)};
        strict->SetBestBlock(hash_a);
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!ConnectBlock(block_b, chain.Context(hash_b), &index_b, *strict, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
}

BOOST_AUTO_TEST_SUITE_END()
