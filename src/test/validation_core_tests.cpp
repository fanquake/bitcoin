// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for the pure block validation core. Deliberately no TestingSetup:
// there is no ChainstateManager, no block index, no disk and no cs_main
// here. Everything the functions need is built by hand and passed in.

#include <addresstype.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
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
    //! Median time past by height, for the genesis block, block 1 and a test block at height 2.
    std::vector<int64_t> median_time_past{params->GenesisBlock().nTime, params->GenesisBlock().nTime + 1, params->GenesisBlock().nTime + 1};
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
            .defer_missing_inputs = false,
        };
    }

    //! The context of a block at height 3, on top of the test block `parent` at height 2.
    BlockValidationContext ContextAbove(const uint256& block_hash, const uint256& parent) const
    {
        BlockValidationContext ctx{Context(block_hash)};
        ctx.prev_block_hash = parent;
        ctx.height = HEIGHT + 1;
        ctx.prev_median_time_past = median_time_past.at(HEIGHT);
        return ctx;
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

    //! A block at height 3 on top of the test block `parent`, with `coinbase` and `txs`.
    CBlock MakeBlockAbove(const CBlock& parent, CMutableTransaction coinbase, const std::vector<CTransactionRef>& txs) const
    {
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = parent.GetHash();
        block.nTime = parent.nTime + 1;
        block.nBits = parent.nBits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (const CTransactionRef& tx : txs) block.vtx.push_back(tx);
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

    for (const bool defer : {false, true}) {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationContext ctx{chain.Context(hash)};
        ctx.defer_missing_inputs = defer;
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!ConnectBlock(block, ctx, *view, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
        BOOST_CHECK(result.deferred_txs.empty());
    }
}

//! A block that holds a transaction twice, with a spender of its output between the two, is invalid: the
//! second copy spends inputs the first has spent. With deferral on, that copy must not be taken for a
//! transaction waiting for a lower block, whose outputs would be added over the first copy's.
BOOST_AUTO_TEST_CASE(deferral_rejects_a_transaction_in_the_block_twice)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CTransactionRef twice{Spend(COIN - fee)}; // spends the seeded coin
    CMutableTransaction between_mut;
    between_mut.vin.emplace_back(COutPoint{twice->GetHash(), 0});
    between_mut.vout.emplace_back(COIN - 2 * fee, CScript() << OP_TRUE);
    const CTransactionRef between{MakeTransactionRef(std::move(between_mut))};
    CBlock block{chain.MakeBlock(twice, GetBlockSubsidy(2, chain.Consensus()) + 2 * fee)};
    block.vtx = {block.vtx[0], twice, between, twice};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, chain.Consensus())) ++block.nNonce;
    const uint256 hash{block.GetHash()};

    for (const bool defer : {false, true}) {
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        BlockValidationContext ctx{chain.Context(hash)};
        ctx.defer_missing_inputs = defer;
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!ConnectBlock(block, ctx, *view, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
        BOOST_CHECK(result.deferred_txs.empty());
    }
}

//! A transaction that repeats an earlier one whose output is still unspent, at a height where BIP30 is not
//! checked (as on mainnet after BIP34), with a spend of that output after it. Its inputs are spent, so with
//! deferral it is a candidate; creating its outputs over the unspent coin must not happen. In order the
//! block is rejected for the spent inputs, and with deferral the same.
BOOST_AUTO_TEST_CASE(deferral_rejects_a_repeat_over_an_unspent_coin)
{
    Chain chain;
    CMutableTransaction repeat_mut;
    repeat_mut.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{7}), 0}); // spent long ago
    repeat_mut.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const CTransactionRef repeat{MakeTransactionRef(std::move(repeat_mut))};
    CMutableTransaction spend_mut;
    spend_mut.vin.emplace_back(COutPoint{repeat->GetHash(), 0});
    spend_mut.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const CTransactionRef spend{MakeTransactionRef(std::move(spend_mut))};
    CBlock block{chain.MakeBlock(repeat, GetBlockSubsidy(2, chain.Consensus()))};
    block.vtx = {block.vtx[0], repeat, spend};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, chain.Consensus())) ++block.nNonce;
    const uint256 hash{block.GetHash()};

    for (const bool defer : {false, true}) {
        // The earlier copy's output, in the view as a prefetch of the spend's input puts it there.
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        view->AddCoin(COutPoint{repeat->GetHash(), 0}, Coin{repeat->vout[0], /*nHeightIn=*/1, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
        // BIP30 is not checked for the block: the context says so.
        BlockValidationContext ctx{chain.Context(hash, /*check_scripts=*/true, /*enforce_bip30=*/false)};
        ctx.defer_missing_inputs = defer;
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!ConnectBlock(block, ctx, *view, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
        BOOST_CHECK(result.deferred_txs.empty());
        BOOST_CHECK(view->HaveCoin(COutPoint{repeat->GetHash(), 0}));
    }
}

//! The same repeat whose inputs the view still shows unspent: a lower block in flight created the earlier
//! copy's outputs and has not applied its spends yet (a deferred transaction waiting for its continuation).
//! The copy is not deferred and its inputs check out, and creating its outputs over the visible coin must
//! not happen either: with deferral the block fails, and the window connects it again alone.
BOOST_AUTO_TEST_CASE(deferral_rejects_a_copy_over_an_unspent_coin)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CTransactionRef copy{Spend(COIN - fee)}; // spends the seeded coin
    CMutableTransaction spend_mut;
    spend_mut.vin.emplace_back(COutPoint{copy->GetHash(), 0});
    spend_mut.vout.emplace_back(COIN - 2 * fee, CScript() << OP_TRUE);
    const CTransactionRef spend{MakeTransactionRef(std::move(spend_mut))};
    CBlock block{chain.MakeBlock(copy, GetBlockSubsidy(2, chain.Consensus()) + 2 * fee)};
    block.vtx = {block.vtx[0], copy, spend};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, chain.Consensus())) ++block.nNonce;
    const uint256 hash{block.GetHash()};

    // The seeded coin unspent and the earlier copy's output beside it, as a view of the window shows them.
    auto view{SeededView(chain, CScript() << OP_TRUE)};
    view->AddCoin(COutPoint{copy->GetHash(), 0}, Coin{copy->vout[0], /*nHeightIn=*/2, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
    // BIP30 is not checked for the block: the context says so.
    BlockValidationContext ctx{chain.Context(hash, /*check_scripts=*/true, /*enforce_bip30=*/false)};
    ctx.defer_missing_inputs = true;
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_CHECK(!ConnectBlock(block, ctx, *view, nullptr, chain.cache, false, state, result));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    BOOST_CHECK(result.deferred_txs.empty());
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

    CMutableTransaction spend_b_mut;
    spend_b_mut.vin.emplace_back(COutPoint{spend_a->GetHash(), 0});
    spend_b_mut.vout.emplace_back(COIN - 2 * fee, CScript() << OP_TRUE);
    const CTransactionRef spend_b{MakeTransactionRef(std::move(spend_b_mut))};
    CMutableTransaction coinbase_b;
    coinbase_b.vin.emplace_back(COutPoint{}, CScript() << 3 << OP_0);
    coinbase_b.vout.emplace_back(GetBlockSubsidy(3, chain.Consensus()) + fee, CScript() << OP_TRUE);
    const CBlock block_b{chain.MakeBlockAbove(block_a, std::move(coinbase_b), {spend_b})};
    const uint256 hash_b{block_b.GetHash()};

    // In order, for reference.
    auto in_order{SeededView(chain, CScript() << OP_TRUE)};
    {
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_REQUIRE(ConnectBlock(block_a, chain.Context(hash_a), *in_order, nullptr, chain.cache, false, state, result));
        BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, chain.ContextAbove(hash_b, hash_a), *in_order, nullptr, chain.cache, false, state, result), state.ToString());
    }

    // B first, with deferral allowed: its transaction is deferred, its outputs exist, the coinbase check waits.
    auto view{SeededView(chain, CScript() << OP_TRUE)};
    view->SetBestBlock(hash_a); // B's parent is A; the view claims to be at A though A's changes are missing
    BlockValidationContext ctx_b{chain.ContextAbove(hash_b, hash_a)};
    ctx_b.defer_missing_inputs = true;
    BlockValidationState state_b;
    ConnectBlockResult result_b;
    BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, ctx_b, *view, nullptr, chain.cache, false, state_b, result_b), state_b.ToString());
    BOOST_REQUIRE_EQUAL(result_b.deferred_txs.size(), 1U);
    BOOST_CHECK_EQUAL(result_b.deferred_txs[0], 1U);
    BOOST_CHECK(result_b.coinbase_check_deferred);
    BOOST_CHECK(view->HaveCoin(COutPoint{spend_b->GetHash(), 0}));
    BOOST_CHECK(result_b.undo.vtxundo.size() == 1 && result_b.undo.vtxundo[0].vprevout.empty());

    // Now A's changes arrive (as the lower block's apply would bring them), and B is finished.
    view->AddCoin(COutPoint{spend_a->GetHash(), 0}, Coin{spend_a->vout[0], 2, false}, false);
    view->SpendCoin(SEEDED_OUT);
    BOOST_REQUIRE_MESSAGE(FinishDeferredTransactions(block_b, ctx_b, *view, chain.cache, state_b, result_b), state_b.ToString());
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
        BOOST_REQUIRE(ConnectBlock(block_b, ctx_b, *lonely, nullptr, chain.cache, false, state, result));
        BOOST_CHECK(!FinishDeferredTransactions(block_b, ctx_b, *lonely, chain.cache, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
    // Without deferral the block is invalid at once.
    {
        auto strict{SeededView(chain, CScript() << OP_TRUE)};
        strict->SetBestBlock(hash_a);
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!ConnectBlock(block_b, chain.ContextAbove(hash_b, hash_a), *strict, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
}

//! A deferred transaction's checks run once the lower block's changes are in, and fail as validating in order
//! fails, with the same reason: a script that does not verify, a spend of a coinbase that is not mature, a
//! coinbase that claims more than the subsidy and the fees, which is checked only once every fee is known, a
//! relative lock time (BIP68) the input's age does not meet, in blocks and in time, and a block whose
//! signature operations exceed the limit only with the P2SH input's counted, be the others the deferred
//! transaction's own or shared with a transaction that was not deferred: the count goes on across the split.
BOOST_AUTO_TEST_CASE(deferred_checks_fail_as_in_order)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    struct Variant {
        const char* name;
        CScript a_output;   //!< the script of the output block A creates and block B spends
        bool spend_coinbase; //!< B spends A's coinbase output instead
        CAmount extra;       //!< what B's coinbase claims beyond the subsidy and its fee
        uint32_t sequence{CTxIn::SEQUENCE_FINAL}; //!< the sequence of B's input (B is version 2)
        CScript script_sig{};                     //!< B's input script
        unsigned int legacy_sigops{0};           //!< signature operations in an extra output of B's spend
        CAmount overspend{0};                    //!< what B's spend pays beyond its input's value
        unsigned int coinbase_sigops{0};         //!< signature operations in an extra output of B's coinbase
        const char* reason{nullptr};             //!< the reject reason expected, where the case rests on it
    };
    // A P2SH output whose redeem script counts one signature operation; it is never run, as the sigops check
    // comes first.
    const CScript redeem{CScript() << OP_CHECKSIG};
    const CScript redeem_push{CScript() << std::vector<unsigned char>(redeem.begin(), redeem.end())};
    const std::vector<Variant> variants{
        {"script", CScript() << OP_FALSE, false, 0},
        {"immature coinbase", CScript() << OP_TRUE, true, 0},
        {"coinbase amount", CScript() << OP_TRUE, false, 1},
        {"relative lock time", CScript() << OP_TRUE, false, 0, /*sequence=*/10},
        // One unit of 512 seconds past the median time of the block before A's, which the context's callback
        // gives: the chain's blocks are seconds apart.
        {"relative lock time in seconds", CScript() << OP_TRUE, false, 0, /*sequence=*/CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | 1},
        // The block's legacy operations reach the limit exactly (CheckBlock() allows it), the P2SH input's one
        // more passes it.
        {"signature operations", GetScriptForDestination(ScriptHash(redeem)), false, 0, CTxIn::SEQUENCE_FINAL, redeem_push,
         MAX_BLOCK_SIGOPS_COST / WITNESS_SCALE_FACTOR},
        {"inputs below outputs", CScript() << OP_TRUE, false, 0, CTxIn::SEQUENCE_FINAL, {}, 0, /*overspend=*/fee + 1},
        // The same limit reached by the coinbase, which is not deferred, and the deferred transaction
        // together, half each: neither side of the split passes it alone, even with the P2SH input's one.
        {"signature operations on both sides of the split", GetScriptForDestination(ScriptHash(redeem)), false, 0,
         CTxIn::SEQUENCE_FINAL, redeem_push, MAX_BLOCK_SIGOPS_COST / WITNESS_SCALE_FACTOR / 2, /*overspend=*/0,
         /*coinbase_sigops=*/MAX_BLOCK_SIGOPS_COST / WITNESS_SCALE_FACTOR / 2, /*reason=*/"bad-blk-sigops"},
    };
    for (const Variant& variant : variants) {
        BOOST_TEST_MESSAGE(variant.name);
        // Block A at height 2 spends the seeded coin.
        CMutableTransaction spend_a_mut;
        spend_a_mut.vin.emplace_back(SEEDED_OUT);
        spend_a_mut.vout.emplace_back(COIN - fee, variant.a_output);
        const CTransactionRef spend_a{MakeTransactionRef(std::move(spend_a_mut))};
        const CBlock block_a{chain.MakeBlock(spend_a, GetBlockSubsidy(2, chain.Consensus()) + fee)};
        const uint256 hash_a{block_a.GetHash()};

        // Block B at height 3 spends A's output, or A's coinbase.
        const CTransactionRef& source{variant.spend_coinbase ? block_a.vtx[0] : spend_a};
        CMutableTransaction spend_b_mut;
        spend_b_mut.version = 2;
        spend_b_mut.vin.emplace_back(COutPoint{source->GetHash(), 0}, variant.script_sig, variant.sequence);
        spend_b_mut.vout.emplace_back(source->vout[0].nValue - fee + variant.overspend, CScript() << OP_TRUE);
        if (variant.legacy_sigops > 0) {
            CScript checksigs;
            for (unsigned int i{0}; i < variant.legacy_sigops; ++i) checksigs << OP_CHECKSIG;
            spend_b_mut.vout.emplace_back(0, checksigs);
        }
        const CTransactionRef spend_b{MakeTransactionRef(std::move(spend_b_mut))};
        CMutableTransaction coinbase_b;
        coinbase_b.vin.emplace_back(COutPoint{}, CScript() << 3 << OP_0);
        coinbase_b.vout.emplace_back(GetBlockSubsidy(3, chain.Consensus()) + fee + variant.extra, CScript() << OP_TRUE);
        if (variant.coinbase_sigops > 0) {
            CScript checksigs;
            for (unsigned int i{0}; i < variant.coinbase_sigops; ++i) checksigs << OP_CHECKSIG;
            coinbase_b.vout.emplace_back(0, checksigs);
        }
        const CBlock block_b{chain.MakeBlockAbove(block_a, std::move(coinbase_b), {spend_b})};
        const uint256 hash_b{block_b.GetHash()};

        // In order: A connects, B fails.
        std::string in_order_reason;
        {
            auto view{SeededView(chain, CScript() << OP_TRUE)};
            BlockValidationState state;
            ConnectBlockResult result;
            BOOST_REQUIRE(ConnectBlock(block_a, chain.Context(hash_a), *view, nullptr, chain.cache, false, state, result));
            ConnectBlockResult result_b;
            BOOST_REQUIRE(!ConnectBlock(block_b, chain.ContextAbove(hash_b, hash_a), *view, nullptr, chain.cache, false, state, result_b));
            in_order_reason = state.GetRejectReason();
            BOOST_REQUIRE(!in_order_reason.empty());
            BOOST_TEST_MESSAGE("in order: " << in_order_reason);
            if (variant.reason) BOOST_CHECK_EQUAL(in_order_reason, variant.reason);
        }

        // B first, deferred: its first half passes; once A's changes are in, the second half fails the same way.
        auto view{SeededView(chain, CScript() << OP_TRUE)};
        view->SetBestBlock(hash_a);
        BlockValidationContext ctx_b{chain.ContextAbove(hash_b, hash_a)};
        ctx_b.defer_missing_inputs = true;
        BlockValidationState state_b;
        ConnectBlockResult result_b;
        BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, ctx_b, *view, nullptr, chain.cache, false, state_b, result_b), state_b.ToString());
        BOOST_REQUIRE_EQUAL(result_b.deferred_txs.size(), 1U);
        view->AddCoin(COutPoint{spend_a->GetHash(), 0}, Coin{spend_a->vout[0], 2, false}, false);
        view->AddCoin(COutPoint{block_a.vtx[0]->GetHash(), 0}, Coin{block_a.vtx[0]->vout[0], 2, true}, false);
        view->SpendCoin(SEEDED_OUT);
        BOOST_CHECK(!FinishDeferredTransactions(block_b, ctx_b, *view, chain.cache, state_b, result_b));
        BOOST_CHECK_EQUAL(state_b.GetRejectReason(), in_order_reason);
    }
}

//! Deferred transactions among others of the block: a transaction that spends a deferred one's output is
//! not deferred itself, a deferred one may spend what the block made before it, and the block ends with the
//! fees, the undo data and the coins of validating in order. A coin spent on both sides of the split is
//! spent twice, whichever of the two transactions waits.
BOOST_AUTO_TEST_CASE(deferred_transactions_among_others)
{
    Chain chain;
    const CAmount fee{COIN / 100};
    const CScript anyone{CScript() << OP_TRUE};
    const COutPoint other{Txid::FromUint256(uint256{7}), 0};
    const auto seeded{[&] {
        auto view{SeededView(chain, anyone)};
        view->AddCoin(other, Coin{CTxOut{COIN, anyone}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
        return view;
    }};
    const auto pay{[&](const std::vector<COutPoint>& inputs, CAmount value) {
        CMutableTransaction tx;
        for (const COutPoint& input : inputs) tx.vin.emplace_back(input);
        tx.vout.emplace_back(value, anyone);
        return MakeTransactionRef(std::move(tx));
    }};

    // Block A at height 2 spends the seeded coin into two outputs.
    CMutableTransaction spend_a_mut;
    spend_a_mut.vin.emplace_back(SEEDED_OUT);
    spend_a_mut.vout.emplace_back(COIN / 4, anyone);
    spend_a_mut.vout.emplace_back(COIN / 4, anyone);
    const CTransactionRef spend_a{MakeTransactionRef(std::move(spend_a_mut))};
    const CBlock block_a{chain.MakeBlock(spend_a, GetBlockSubsidy(2, chain.Consensus()) + COIN / 2)};
    const uint256 hash_a{block_a.GetHash()};
    const COutPoint a0{spend_a->GetHash(), 0};
    const COutPoint a1{spend_a->GetHash(), 1};

    const auto block_above_a{[&](const std::vector<CTransactionRef>& txs, CAmount fees) {
        CMutableTransaction coinbase;
        coinbase.vin.emplace_back(COutPoint{}, CScript() << 3 << OP_0);
        coinbase.vout.emplace_back(GetBlockSubsidy(3, chain.Consensus()) + fees, anyone);
        return chain.MakeBlockAbove(block_a, std::move(coinbase), txs);
    }};
    const auto bring_a{[&](CCoinsViewCache& view) {
        view.AddCoin(a0, Coin{spend_a->vout[0], 2, false}, false);
        view.AddCoin(a1, Coin{spend_a->vout[1], 2, false}, false);
        view.SpendCoin(SEEDED_OUT);
    }};

    // Block B at height 3: t1 spends a coin below A, t2 an output of A (deferred), t3 the outputs of t2 and
    // t1, t4 an output of A and that of t3 (deferred).
    const CTransactionRef t1{pay({other}, COIN - fee)};
    const CTransactionRef t2{pay({a0}, COIN / 4 - fee)};
    const CTransactionRef t3{pay({COutPoint{t2->GetHash(), 0}, COutPoint{t1->GetHash(), 0}}, COIN + COIN / 4 - 3 * fee)};
    const CTransactionRef t4{pay({a1, COutPoint{t3->GetHash(), 0}}, COIN + COIN / 2 - 4 * fee)};
    const CBlock block_b{block_above_a({t1, t2, t3, t4}, 4 * fee)};
    const uint256 hash_b{block_b.GetHash()};

    // In order, for reference.
    auto in_order{seeded()};
    ConnectBlockResult reference;
    {
        BlockValidationState state;
        ConnectBlockResult result_a;
        BOOST_REQUIRE_MESSAGE(ConnectBlock(block_a, chain.Context(hash_a), *in_order, nullptr, chain.cache, false, state, result_a), state.ToString());
        BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, chain.ContextAbove(hash_b, hash_a), *in_order, nullptr, chain.cache, false, state, reference), state.ToString());
    }
    BOOST_CHECK_EQUAL(reference.fees, 4 * fee);

    // B first: t2 and t4 wait, t3 does not.
    auto view{seeded()};
    view->SetBestBlock(hash_a);
    BlockValidationContext ctx_b{chain.ContextAbove(hash_b, hash_a)};
    ctx_b.defer_missing_inputs = true;
    BlockValidationState state_b;
    ConnectBlockResult result_b;
    BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, ctx_b, *view, nullptr, chain.cache, false, state_b, result_b), state_b.ToString());
    BOOST_REQUIRE_EQUAL(result_b.deferred_txs.size(), 2U);
    BOOST_CHECK_EQUAL(result_b.deferred_txs[0], 2U);
    BOOST_CHECK_EQUAL(result_b.deferred_txs[1], 4U);
    BOOST_CHECK(result_b.coinbase_check_deferred);

    bring_a(*view);
    BOOST_REQUIRE_MESSAGE(FinishDeferredTransactions(block_b, ctx_b, *view, chain.cache, state_b, result_b), state_b.ToString());
    BOOST_CHECK(result_b.deferred_txs.empty());
    BOOST_CHECK(!result_b.coinbase_check_deferred);
    BOOST_CHECK_EQUAL(result_b.fees, reference.fees);
    BOOST_REQUIRE_EQUAL(result_b.undo.vtxundo.size(), reference.undo.vtxundo.size());
    for (size_t i{0}; i < reference.undo.vtxundo.size(); ++i) {
        const std::vector<Coin>& got{result_b.undo.vtxundo[i].vprevout};
        const std::vector<Coin>& want{reference.undo.vtxundo[i].vprevout};
        BOOST_REQUIRE_EQUAL(got.size(), want.size());
        for (size_t j{0}; j < want.size(); ++j) {
            BOOST_CHECK(got[j].out == want[j].out);
            BOOST_CHECK_EQUAL(got[j].nHeight, want[j].nHeight);
            BOOST_CHECK_EQUAL(got[j].fCoinBase, want[j].fCoinBase);
        }
    }
    for (const COutPoint& out : {SEEDED_OUT, other, a0, a1, COutPoint{t1->GetHash(), 0}, COutPoint{t2->GetHash(), 0},
                                 COutPoint{t3->GetHash(), 0}, COutPoint{t4->GetHash(), 0}, COutPoint{block_b.vtx[0]->GetHash(), 0}}) {
        BOOST_CHECK_EQUAL(view->HaveCoin(out), in_order->HaveCoin(out));
    }
    BOOST_CHECK(view->HaveCoin(COutPoint{t4->GetHash(), 0}));

    // A coin spent by a transaction that is checked at once and by one that waits, in both orders.
    const CTransactionRef plain{pay({other}, COIN - fee)};
    const CTransactionRef waiting{pay({a1, other}, COIN + COIN / 4 - fee)};
    for (const bool waiting_first : {false, true}) {
        const CBlock twice{waiting_first ? block_above_a({waiting, plain}, 2 * fee) : block_above_a({plain, waiting}, 2 * fee)};
        BlockValidationContext ctx{chain.ContextAbove(twice.GetHash(), hash_a)};
        ctx.defer_missing_inputs = true;
        auto spent_twice{seeded()};
        spent_twice->SetBestBlock(hash_a);
        BlockValidationState state;
        ConnectBlockResult result;
        // The first pass passes with the waiting transaction deferred: the spend on the other side of
        // the split is found by the second.
        BOOST_REQUIRE(ConnectBlock(twice, ctx, *spent_twice, nullptr, chain.cache, false, state, result));
        BOOST_REQUIRE(!result.deferred_txs.empty());
        bring_a(*spent_twice);
        BOOST_CHECK(!FinishDeferredTransactions(twice, ctx, *spent_twice, chain.cache, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
}

//! A transaction whose inputs the view holds is checked at once in a block that may defer, without a
//! second look for a missing input. What the check finds wrong with inputs that are there, it finds the
//! same with deferral on and off, and a valid block gives the same result.
BOOST_AUTO_TEST_CASE(present_inputs_check_the_same_with_deferral)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    struct Case {
        CAmount coin_value;
        bool coin_is_coinbase;
        CAmount spend_value;
        std::string reject_reason; //!< empty for a valid block
    };
    const std::vector<Case> cases{
        {COIN, false, COIN - fee, ""},
        {COIN, true, COIN - fee, "bad-txns-premature-spend-of-coinbase"},
        {COIN, false, COIN + 1, "bad-txns-in-belowout"},
        {MAX_MONEY + 1, false, COIN - fee, "bad-txns-inputvalues-outofrange"},
    };
    for (const Case& c : cases) {
        const CBlock block{chain.MakeBlock(Spend(c.spend_value), GetBlockSubsidy(HEIGHT, chain.Consensus()) + fee)};
        const uint256 hash{block.GetHash()};
        // Without deferral, with it, and with it and a word on pending blocks' transactions to ask for.
        for (const int mode : {0, 1, 2}) {
            auto view{std::make_unique<CCoinsViewCache>(&CoinsViewEmpty::Get())};
            view->SetBestBlock(chain.prev_hash);
            view->AddCoin(SEEDED_OUT, Coin{CTxOut{c.coin_value, CScript() << OP_TRUE}, /*nHeightIn=*/1, c.coin_is_coinbase}, /*possible_overwrite=*/false);
            BlockValidationContext ctx{chain.Context(hash)};
            ctx.defer_missing_inputs = mode > 0;
            if (mode == 2) ctx.made_by_pending_block = [](const Txid&) { return false; };
            BlockValidationState state;
            ConnectBlockResult result;
            const bool connected{chain.Connect(block, ctx, *view, state, result)};
            BOOST_CHECK_EQUAL(connected, c.reject_reason.empty());
            BOOST_CHECK_EQUAL(state.GetRejectReason(), c.reject_reason);
            BOOST_CHECK(result.deferred_txs.empty());
            if (connected) {
                BOOST_CHECK_EQUAL(result.fees, fee);
                BOOST_CHECK(!view->HaveCoin(SEEDED_OUT));
                BOOST_CHECK(view->HaveCoin(COutPoint{block.vtx[1]->GetHash(), 0}));
            }
        }
    }
}

//! In a block that may defer, an input the view does not hold yet and whose transaction a lower block is
//! said to hold (BlockValidationContext::made_by_pending_block) is not looked up: its transaction is
//! deferred, and checked against the input itself when the block is finished. The word is a hint: given
//! for a coin that is there all along, the block's result is the same as without it.
BOOST_AUTO_TEST_CASE(pending_block_hint_defers_without_a_lookup)
{
    Chain chain;
    const CAmount fee{COIN / 10};
    const CTransactionRef spend_a{Spend(COIN - fee)};
    const CBlock block_a{chain.MakeBlock(spend_a, GetBlockSubsidy(2, chain.Consensus()) + fee)};
    const uint256 hash_a{block_a.GetHash()};
    const COutPoint out_a{spend_a->GetHash(), 0};

    CMutableTransaction spend_b_mut;
    spend_b_mut.vin.emplace_back(out_a);
    spend_b_mut.vout.emplace_back(COIN - 2 * fee, CScript() << OP_TRUE);
    const CTransactionRef spend_b{MakeTransactionRef(std::move(spend_b_mut))};
    CMutableTransaction coinbase_b;
    coinbase_b.vin.emplace_back(COutPoint{}, CScript() << 3 << OP_0);
    coinbase_b.vout.emplace_back(GetBlockSubsidy(3, chain.Consensus()) + fee, CScript() << OP_TRUE);
    const CBlock block_b{chain.MakeBlockAbove(block_a, std::move(coinbase_b), {spend_b})};
    const uint256 hash_b{block_b.GetHash()};

    for (const bool hint : {false, true}) {
        for (const bool fetched : {false, true}) {
            // A's output is below the view: a lookup finds it.
            CCoinsViewCache below{&CoinsViewEmpty::Get()};
            below.AddCoin(out_a, Coin{spend_a->vout[0], 2, false}, false);
            CCoinsViewCache view{&below};
            view.SetBestBlock(hash_a);
            if (fetched) BOOST_REQUIRE(view.HaveCoin(out_a));
            BOOST_CHECK_EQUAL(view.HaveCoinInCache(out_a), fetched);

            int asked{0};
            BlockValidationContext ctx{chain.ContextAbove(hash_b, hash_a)};
            ctx.defer_missing_inputs = true;
            ctx.made_by_pending_block = [&](const Txid& txid) {
                ++asked;
                return hint && txid == spend_a->GetHash();
            };
            BlockValidationState state;
            ConnectBlockResult result;
            BOOST_REQUIRE_MESSAGE(ConnectBlock(block_b, ctx, view, nullptr, chain.cache, false, state, result), state.ToString());
            // The hint is asked only for a coin the view does not hold, and only then defers.
            const bool deferred{hint && !fetched};
            BOOST_CHECK_EQUAL(asked, fetched ? 0 : 1);
            BOOST_CHECK_EQUAL(result.deferred_txs.size(), deferred ? 1U : 0U);
            BOOST_CHECK_EQUAL(result.coinbase_check_deferred, deferred);
            if (deferred) {
                BOOST_CHECK(!view.HaveCoinInCache(out_a)); // not looked up
                BOOST_REQUIRE_MESSAGE(FinishDeferredTransactions(block_b, ctx, view, chain.cache, state, result), state.ToString());
            }
            BOOST_CHECK_EQUAL(result.fees, fee);
            BOOST_REQUIRE_EQUAL(result.undo.vtxundo.size(), 1U);
            BOOST_REQUIRE_EQUAL(result.undo.vtxundo[0].vprevout.size(), 1U);
            BOOST_CHECK_EQUAL(result.undo.vtxundo[0].vprevout[0].out.nValue, COIN - fee);
            BOOST_CHECK(!view.HaveCoin(out_a));
            BOOST_CHECK(view.HaveCoin(COutPoint{spend_b->GetHash(), 0}));
        }
    }

    // The hint given for a transaction whose coin never comes: the block fails when it is finished, as a
    // deferred transaction's missing input does.
    {
        CCoinsViewCache view{&CoinsViewEmpty::Get()};
        view.SetBestBlock(hash_a);
        BlockValidationContext ctx{chain.ContextAbove(hash_b, hash_a)};
        ctx.defer_missing_inputs = true;
        ctx.made_by_pending_block = [](const Txid&) { return true; };
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_REQUIRE(ConnectBlock(block_b, ctx, view, nullptr, chain.cache, false, state, result));
        BOOST_CHECK_EQUAL(result.deferred_txs.size(), 1U);
        BOOST_CHECK(!FinishDeferredTransactions(block_b, ctx, view, chain.cache, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
}

BOOST_AUTO_TEST_SUITE_END()
