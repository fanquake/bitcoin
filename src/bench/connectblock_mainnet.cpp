// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Connect a real mainnet block (413567, 1557 transactions) against its real
// prevouts, which the block's undo data supplies, through the pure
// ConnectBlock(). No node: the block index entries are built here, the coins
// are seeded into a cache over an empty view, and each iteration connects
// into a fresh child of that cache, which is what connecting into a child of
// the tip cache costs today.
//
// Two variants. NoScripts measures the UTXO-set side of connecting a block:
// the lookups, the input checks, the sigop count, the undo data and the
// writes into the view. WarmScripts adds script verification with a
// ValidationCache that is warm after the first iteration, which is the state
// a block reconstructed from the mempool finds at the tip; it runs the core
// in its just_check mode, the one that keeps cache entries on a hit, so the
// cache stays warm across iterations.

#include <bench/bench.h>
#include <bench/data/block413567.raw.h>
#include <bench/data/block413567_undo.raw.h>
#include <chain.h>
#include <coins.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <script/sigcache.h>
#include <validation.h>

#include <cassert>
#include <memory>
#include <vector>

namespace {

constexpr int BLOCK_HEIGHT{413567};

struct MainnetBlock {
    std::unique_ptr<const CChainParams> params{CChainParams::Main()};
    CBlock block;
    CBlockUndo undo;
    uint256 block_hash;
    uint256 dummy_hash{1};
    uint256 bip34_hash;
    //! Every block index entry from genesis to the block, so that GetAncestor() and GetMedianTimePast() walk
    //! real pprev and pskip links. Only the heights, the BIP34 ancestor's hash and the parent's hash matter.
    std::unique_ptr<CBlockIndex[]> chain{std::make_unique<CBlockIndex[]>(BLOCK_HEIGHT + 1)};
    //! The block's inputs, seeded with their real coins.
    CCoinsViewCache seeded{&CoinsViewEmpty::Get()};
    ValidationCache validation_cache{DEFAULT_SCRIPT_EXECUTION_CACHE_BYTES, DEFAULT_SIGNATURE_CACHE_BYTES};

    MainnetBlock()
    {
        SpanReader{benchmark::data::block413567} >> TX_WITH_WITNESS(block);
        SpanReader{benchmark::data::block413567_undo} >> undo;
        block_hash = block.GetHash();
        bip34_hash = params->GetConsensus().BIP34Hash;
        Assert(undo.vtxundo.size() + 1 == block.vtx.size());

        for (int height{0}; height <= BLOCK_HEIGHT; ++height) {
            CBlockIndex& index{chain[height]};
            index.nHeight = height;
            index.nTime = static_cast<uint32_t>(1231006505 + static_cast<int64_t>(height) * 600);
            index.phashBlock = &dummy_hash;
            if (height > 0) {
                index.pprev = &chain[height - 1];
                index.BuildSkip();
            }
        }
        chain[params->GetConsensus().BIP34Height].phashBlock = &bip34_hash;
        chain[BLOCK_HEIGHT - 1].phashBlock = &block.hashPrevBlock;
        chain[BLOCK_HEIGHT].phashBlock = &block_hash;
        chain[BLOCK_HEIGHT].nTime = block.nTime;
        chain[BLOCK_HEIGHT].nBits = block.nBits;
        chain[BLOCK_HEIGHT].nVersion = block.nVersion;

        seeded.SetBestBlock(block.hashPrevBlock);
        for (size_t i{1}; i < block.vtx.size(); ++i) {
            const CTransaction& tx{*block.vtx[i]};
            const CTxUndo& tx_undo{undo.vtxundo[i - 1]};
            Assert(tx_undo.vprevout.size() == tx.vin.size());
            for (size_t j{0}; j < tx.vin.size(); ++j) {
                seeded.AddCoin(tx.vin[j].prevout, Coin{tx_undo.vprevout[j]}, /*possible_overwrite=*/false);
            }
        }
    }

    BlockValidationContext Context(bool check_scripts) const
    {
        // Mainnet at height 413567: BIP34, BIP66 and BIP65 active; BIP68/112/113 and segwit not yet.
        const DeploymentFlags deployments{.heightincb = true, .dersig = true, .cltv = true, .csv = false, .segwit = false};
        return BlockValidationContext{
            .params = params->GetConsensus(),
            .now = NodeClock::now(),
            .deployments = deployments,
            .script_flags = GetBlockScriptFlags(block_hash, deployments, params->GetConsensus()),
            .check_scripts = check_scripts,
            .defer_missing_inputs = false,
        };
    }
};

void ConnectMainnetBlock(benchmark::Bench& bench, bool check_scripts)
{
    MainnetBlock data;
    const BlockValidationContext ctx{data.Context(check_scripts)};
    CBlockIndex* index{&data.chain[BLOCK_HEIGHT]};
    bench.unit("block").run([&] {
        CCoinsViewCache view{&data.seeded};
        BlockValidationState state;
        ConnectBlockResult result;
        const bool ok{ConnectBlock(data.block, ctx, index, view, /*check_queue=*/nullptr, data.validation_cache,
                                   /*just_check=*/check_scripts, state, result)};
        assert(ok);
        ankerl::nanobench::doNotOptimizeAway(result.num_inputs);
    });
}

void ConnectBlockMainnetNoScripts(benchmark::Bench& bench) { ConnectMainnetBlock(bench, /*check_scripts=*/false); }
void ConnectBlockMainnetWarmScripts(benchmark::Bench& bench) { ConnectMainnetBlock(bench, /*check_scripts=*/true); }

} // namespace

BENCHMARK(ConnectBlockMainnetNoScripts);
BENCHMARK(ConnectBlockMainnetWarmScripts);
