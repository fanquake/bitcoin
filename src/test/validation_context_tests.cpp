// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The node's BlockValidationContext against the block index values that the
// validation functions read directly before the context existed.

#include <chain.h>
#include <consensus/params.h>
#include <deploymentstatus.h>
#include <script/interpreter.h>
#include <script/verify_flags.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(validation_context_tests)

BOOST_FIXTURE_TEST_CASE(context_matches_the_block_index, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    LOCK(cs_main);
    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    BOOST_REQUIRE(tip);
    const uint256 hash{uint256::ONE};
    const BlockValidationContext ctx{chainman.MakeBlockValidationContext(hash, tip)};

    BOOST_CHECK(&ctx.params == &chainman.GetConsensus());
    BOOST_CHECK(ctx.block_hash == hash);
    BOOST_CHECK(ctx.prev_block_hash == tip->GetBlockHash());
    BOOST_CHECK_EQUAL(ctx.height, tip->nHeight + 1);
    BOOST_CHECK_EQUAL(ctx.prev_median_time_past, tip->GetMedianTimePast());
    BOOST_CHECK(ctx.check_scripts);
    BOOST_CHECK_EQUAL(ctx.deployments.heightincb, DeploymentActiveAfter(tip, chainman, Consensus::DEPLOYMENT_HEIGHTINCB));
    BOOST_CHECK_EQUAL(ctx.deployments.dersig, DeploymentActiveAfter(tip, chainman, Consensus::DEPLOYMENT_DERSIG));
    BOOST_CHECK_EQUAL(ctx.deployments.cltv, DeploymentActiveAfter(tip, chainman, Consensus::DEPLOYMENT_CLTV));
    BOOST_CHECK_EQUAL(ctx.deployments.csv, DeploymentActiveAfter(tip, chainman, Consensus::DEPLOYMENT_CSV));
    BOOST_CHECK_EQUAL(ctx.deployments.segwit, DeploymentActiveAfter(tip, chainman, Consensus::DEPLOYMENT_SEGWIT));
    BOOST_CHECK(ctx.script_flags == GetBlockScriptFlags(hash, ctx.deployments, chainman.GetConsensus()));
    // Regtest has no block that BIP34 would vouch for, so BIP30 is checked.
    BOOST_CHECK(ctx.enforce_bip30);
    for (const int height : {0, 1, tip->nHeight / 2, tip->nHeight}) {
        BOOST_CHECK_EQUAL(ctx.median_time_past_at(height), tip->GetAncestor(height)->GetMedianTimePast());
    }

    // The genesis block: no previous block, nothing to connect, no BIP30 check.
    const BlockValidationContext genesis{chainman.MakeBlockValidationContext(chainman.GetConsensus().hashGenesisBlock, nullptr)};
    BOOST_CHECK(genesis.prev_block_hash.IsNull());
    BOOST_CHECK_EQUAL(genesis.height, 0);
    BOOST_CHECK_EQUAL(genesis.prev_median_time_past, 0);
    BOOST_CHECK(!genesis.enforce_bip30);
}

namespace {
struct MainNetSetup : public TestingSetup {
    MainNetSetup() : TestingSetup{ChainType::MAIN} {}
};
} // namespace

// The two blocks of the main chain that repeat a coinbase are the only ones exempt from
// BIP30 below the BIP34 height.
BOOST_FIXTURE_TEST_CASE(bip30_exceptions, MainNetSetup)
{
    auto& chainman{*Assert(m_node.chainman)};
    LOCK(cs_main);
    const uint256 prev_hash{uint256::ONE};
    CBlockIndex prev;
    prev.phashBlock = &prev_hash;

    prev.nHeight = 91841;
    const uint256 first_repeat{"00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec"};
    BOOST_CHECK(!chainman.MakeBlockValidationContext(first_repeat, &prev).enforce_bip30);
    BOOST_CHECK(chainman.MakeBlockValidationContext(uint256{2}, &prev).enforce_bip30);

    prev.nHeight = 91879;
    const uint256 second_repeat{"00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721"};
    BOOST_CHECK(!chainman.MakeBlockValidationContext(second_repeat, &prev).enforce_bip30);
    // The exemption is for the block at its height, not for the hash anywhere.
    BOOST_CHECK(chainman.MakeBlockValidationContext(first_repeat, &prev).enforce_bip30);
}

// Above the BIP34 height BIP30 is not checked on the chain that has the known block at that
// height, and is checked on any other.
BOOST_FIXTURE_TEST_CASE(bip30_above_bip34, MainNetSetup)
{
    auto& chainman{*Assert(m_node.chainman)};
    const Consensus::Params& params{chainman.GetConsensus()};
    LOCK(cs_main);
    CBlockIndex at_bip34;
    at_bip34.nHeight = params.BIP34Height;
    at_bip34.phashBlock = &params.BIP34Hash;
    const uint256 prev_hash{uint256::ONE};
    CBlockIndex prev;
    prev.nHeight = params.BIP34Height + 1;
    prev.pprev = &at_bip34;
    prev.phashBlock = &prev_hash;
    BOOST_CHECK(!chainman.MakeBlockValidationContext(uint256{2}, &prev).enforce_bip30);

    const uint256 other_hash{uint256{3}};
    at_bip34.phashBlock = &other_hash;
    BOOST_CHECK(chainman.MakeBlockValidationContext(uint256{2}, &prev).enforce_bip30);
}

// The script flags of a block from its hash and the deployments alone: what each deployment adds, and
// the blocks of the exception table, whose flags are the table's and what the deployments add.
BOOST_FIXTURE_TEST_CASE(script_flags_of_a_block, MainNetSetup)
{
    const Consensus::Params& params{Assert(m_node.chainman)->GetConsensus()};
    const script_verify_flags base{SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_TAPROOT};
    const uint256 hash{uint256::ONE};
    BOOST_REQUIRE(!params.script_flag_exceptions.contains(hash));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{}, params) == base);
    // The height in the coinbase is no script rule.
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.heightincb = true}, params) == base);
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.dersig = true}, params) == (base | SCRIPT_VERIFY_DERSIG));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.cltv = true}, params) == (base | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.csv = true}, params) == (base | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.segwit = true}, params) == (base | SCRIPT_VERIFY_NULLDUMMY));

    BOOST_REQUIRE(!params.script_flag_exceptions.empty());
    for (const auto& [exception, flags] : params.script_flag_exceptions) {
        BOOST_CHECK(flags != base);
        BOOST_CHECK(GetBlockScriptFlags(exception, DeploymentFlags{}, params) == flags);
        BOOST_CHECK(GetBlockScriptFlags(exception, DeploymentFlags{.dersig = true}, params) == (flags | SCRIPT_VERIFY_DERSIG));
    }
}

BOOST_AUTO_TEST_SUITE_END()
