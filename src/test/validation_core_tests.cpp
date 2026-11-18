// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for the pure block validation core. Deliberately no TestingSetup:
// there is no ChainstateManager, no block index, no disk and no cs_main
// here. Everything the functions need is built by hand and passed in.

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

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

BOOST_AUTO_TEST_SUITE(validation_core_tests)

namespace {

//! The blocks under test are at height 2, on top of a block 1 that exists
//! only as a hash and a median time past.
constexpr int HEIGHT{2};

struct Chain {
    std::unique_ptr<const CChainParams> params{CChainParams::RegTest()};
    //! A stand-in for block 1. The test blocks spend a coin "created" in it.
    uint256 prev_hash{2};
    //! Median time past by height, for the genesis block and block 1.
    std::vector<int64_t> median_time_past{params->GenesisBlock().nTime, params->GenesisBlock().nTime + 1};
    ValidationCache cache{1 << 20, 1 << 20};

    const Consensus::Params& Consensus() const { return params->GetConsensus(); }

    BlockValidationContext Context(const uint256& block_hash, bool check_scripts = true, bool enforce_bip30 = true) const
    {
        // Regtest has every buried deployment active from the start.
        const DeploymentFlags deployments{.heightincb = true, .dersig = true, .cltv = true, .csv = true, .segwit = true};
        return BlockValidationContext{
            .params = Consensus(),
            .now = NodeClock::now(),
            .deployments = deployments,
            .script_flags = GetBlockScriptFlags(block_hash, deployments, Consensus()),
            .check_scripts = check_scripts,
            .block_hash = block_hash,
            .prev_block_hash = prev_hash,
            .height = HEIGHT,
            .prev_median_time_past = median_time_past.at(HEIGHT - 1),
            .enforce_bip30 = enforce_bip30,
            .median_time_past_at = [times = median_time_past](int height) { return times.at(height); },
        };
    }

    //! A block at height 2 containing a coinbase and `spend`, mined to regtest difficulty.
    CBlock MakeBlock(const CTransactionRef& spend, CAmount coinbase_value) const
    {
        CMutableTransaction coinbase;
        coinbase.vin.emplace_back(COutPoint{}, CScript() << HEIGHT << OP_0);
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

    bool Connect(const CBlock& block, const BlockValidationContext& ctx, CCoinsViewCache& view, BlockValidationState& state, ConnectBlockResult& result, bool just_check = false)
    {
        return ConnectBlock(block, ctx, view, /*check_queue=*/nullptr, cache, just_check, state, result);
    }
};

//! The coin the test block spends, created at height 1 and seeded into a fresh view below the block.
const COutPoint SEEDED_OUT{Txid::FromUint256(uint256::ONE), 0};

std::unique_ptr<CCoinsViewCache> SeededView(const Chain& chain, const CScript& script_pub_key)
{
    auto view{std::make_unique<CCoinsViewCache>(&CoinsViewEmpty::Get())};
    view->SetBestBlock(chain.prev_hash);
    view->AddCoin(SEEDED_OUT, Coin{CTxOut{COIN, script_pub_key}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
    return view;
}

CTransactionRef Spend(CAmount value, uint32_t sequence = CTxIn::SEQUENCE_FINAL)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(SEEDED_OUT, CScript{}, sequence);
    tx.vout.emplace_back(value, CScript() << OP_TRUE);
    return MakeTransactionRef(std::move(tx));
}

} // namespace

BOOST_AUTO_TEST_CASE(connect_then_disconnect_roundtrip)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CBlock block{chain.MakeBlock(Spend(COIN - fee), GetBlockSubsidy(HEIGHT, chain.Consensus()) + fee)};
    const uint256 hash{block.GetHash()};

    auto view{SeededView(chain, CScript() << OP_TRUE)};
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_REQUIRE_MESSAGE(chain.Connect(block, chain.Context(hash), *view, state, result), state.ToString());
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
    // The coins carry the height from the context.
    BOOST_CHECK_EQUAL(view->AccessCoin(COutPoint{block.vtx[1]->GetHash(), 0}).nHeight, uint32_t{HEIGHT});

    // The returned undo data is all that is needed to roll the view back.
    BOOST_CHECK_EQUAL(DisconnectBlock(block, std::move(result.undo), hash, HEIGHT, *view), DISCONNECT_OK);
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
    const CBlock block{chain.MakeBlock(Spend(COIN - fee), GetBlockSubsidy(HEIGHT, chain.Consensus()) + fee + 1)};
    const uint256 hash{block.GetHash()};

    auto view{SeededView(chain, CScript() << OP_TRUE)};
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_CHECK(!chain.Connect(block, chain.Context(hash), *view, state, result));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amount");
    BOOST_CHECK(!result.failed_in_check_block);
}

BOOST_AUTO_TEST_CASE(script_checks_follow_the_context)
{
    Chain chain;
    const CBlock block{chain.MakeBlock(Spend(COIN), GetBlockSubsidy(HEIGHT, chain.Consensus()))};
    const uint256 hash{block.GetHash()};

    // The seeded coin is unspendable, so the spend fails script verification...
    {
        auto view{SeededView(chain, CScript() << OP_FALSE)};
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!chain.Connect(block, chain.Context(hash, /*check_scripts=*/true), *view, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason().substr(0, 32), "block-script-verify-flag-failed ");
    }
    // ...unless the context says scripts are not to be checked, which is
    // the whole of what -assumevalid means to the core.
    {
        auto view{SeededView(chain, CScript() << OP_FALSE)};
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK_MESSAGE(chain.Connect(block, chain.Context(hash, /*check_scripts=*/false), *view, state, result), state.ToString());
        BOOST_CHECK(view->GetBestBlock() == hash);
    }
}

BOOST_AUTO_TEST_CASE(just_check_leaves_best_block)
{
    Chain chain;
    const CBlock block{chain.MakeBlock(Spend(COIN), GetBlockSubsidy(HEIGHT, chain.Consensus()))};
    const uint256 hash{block.GetHash()};

    auto view{SeededView(chain, CScript() << OP_TRUE)};
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_CHECK_MESSAGE(chain.Connect(block, chain.Context(hash), *view, state, result, /*just_check=*/true), state.ToString());
    BOOST_CHECK(view->GetBestBlock() == chain.prev_hash);
    BOOST_CHECK(!view->HaveCoin(SEEDED_OUT));
}

BOOST_AUTO_TEST_CASE(relative_height_lock_follows_the_context)
{
    Chain chain;
    // The coin is from block 1. A lock of one block is met in block 2, a lock of two is not.
    for (const auto& [blocks, met] : {std::pair{1U, true}, std::pair{2U, false}}) {
        const CBlock block{chain.MakeBlock(Spend(COIN, /*sequence=*/blocks), GetBlockSubsidy(HEIGHT, chain.Consensus()))};
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK_EQUAL(chain.Connect(block, chain.Context(block.GetHash()), *view, state, result), met);
        if (!met) BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
    }
}

BOOST_AUTO_TEST_CASE(relative_time_lock_follows_the_context)
{
    // A lock of one 512 second unit on a coin from block 1 counts from the
    // median time past of the block before it, the genesis block, which the
    // core learns only through the context's lookup. The lock is met once the
    // previous block's median time past is 512 seconds beyond that.
    const uint32_t sequence{CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | 1};
    for (const auto& [elapsed, met] : {std::pair{512, true}, std::pair{511, false}}) {
        Chain chain;
        chain.median_time_past[1] = chain.median_time_past[0] + elapsed;
        const CBlock block{chain.MakeBlock(Spend(COIN, sequence), GetBlockSubsidy(HEIGHT, chain.Consensus()))};
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK_EQUAL(chain.Connect(block, chain.Context(block.GetHash()), *view, state, result), met);
        if (!met) BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
    }
    // The same 512 seconds are not enough when the lookup reports a later
    // median time past for the genesis block.
    {
        Chain chain;
        chain.median_time_past[1] = chain.median_time_past[0] + 512;
        chain.median_time_past[0] += 1;
        const CBlock block{chain.MakeBlock(Spend(COIN, sequence), GetBlockSubsidy(HEIGHT, chain.Consensus()))};
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!chain.Connect(block, chain.Context(block.GetHash()), *view, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
    }
}

BOOST_AUTO_TEST_CASE(bip30_follows_the_context)
{
    Chain chain;
    const CBlock block{chain.MakeBlock(Spend(COIN), GetBlockSubsidy(HEIGHT, chain.Consensus()))};
    const uint256 hash{block.GetHash()};
    const COutPoint coinbase_out{block.vtx[0]->GetHash(), 0};

    // An unspent output of the block's own coinbase is already in the view...
    {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        view->AddCoin(coinbase_out, Coin{block.vtx[0]->vout[0], /*nHeightIn=*/1, /*fCoinBaseIn=*/true}, /*possible_overwrite=*/false);
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!chain.Connect(block, chain.Context(hash), *view, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-BIP30");
    }
    // ...which only a context that does not enforce BIP30 lets the block overwrite.
    {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        view->AddCoin(coinbase_out, Coin{block.vtx[0]->vout[0], /*nHeightIn=*/1, /*fCoinBaseIn=*/true}, /*possible_overwrite=*/false);
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK_MESSAGE(chain.Connect(block, chain.Context(hash, /*check_scripts=*/true, /*enforce_bip30=*/false), *view, state, result), state.ToString());
        BOOST_CHECK_EQUAL(view->AccessCoin(coinbase_out).nHeight, uint32_t{HEIGHT});
    }
}

// A result object can be used for one block after another: every call starts it from
// nothing, and a block that fails CheckBlock() says so in it.
BOOST_AUTO_TEST_CASE(result_is_reset_at_entry)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CBlock block{chain.MakeBlock(Spend(COIN - fee), GetBlockSubsidy(HEIGHT, chain.Consensus()) + fee)};
    ConnectBlockResult result;
    {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationState state;
        BOOST_REQUIRE_MESSAGE(chain.Connect(block, chain.Context(block.GetHash()), *view, state, result), state.ToString());
        BOOST_CHECK(!result.failed_in_check_block);
        BOOST_CHECK_EQUAL(result.num_inputs, 2);
        BOOST_CHECK_EQUAL(result.undo.vtxundo.size(), 1U);
    }
    // The same block with another merkle root fails CheckBlock(), before any coin is looked at.
    CBlock bad{block};
    bad.hashMerkleRoot = uint256::ONE;
    bad.fChecked = bad.m_checked_merkle_root = false; // the copy remembers the checks of the original
    {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationState state;
        BOOST_CHECK(!chain.Connect(bad, chain.Context(bad.GetHash()), *view, state, result));
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK(result.failed_in_check_block);
        BOOST_CHECK_EQUAL(result.num_inputs, 0);
        BOOST_CHECK(result.undo.vtxundo.empty());
        BOOST_CHECK(view->HaveCoin(SEEDED_OUT));
    }
    // And the valid block again, into the result of the failed one.
    {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationState state;
        BOOST_REQUIRE_MESSAGE(chain.Connect(block, chain.Context(block.GetHash()), *view, state, result), state.ToString());
        BOOST_CHECK(!result.failed_in_check_block);
        BOOST_CHECK_EQUAL(result.num_inputs, 2);
        BOOST_CHECK_EQUAL(result.undo.vtxundo.size(), 1U);
    }
}

BOOST_AUTO_TEST_SUITE_END()
