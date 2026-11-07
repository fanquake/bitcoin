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

BOOST_AUTO_TEST_SUITE_END()
