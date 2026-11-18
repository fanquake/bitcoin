// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Connect a real mainnet block (413567, 1557 transactions) against its real
// prevouts, which the block's undo data supplies, through the pure
// ConnectBlock(). No node: the block index entries are built here and the
// coins are seeded over an empty view.
//
// The first two connect into a fresh child of a seeded cache, the core alone.
// NoScripts measures the UTXO-set side of connecting a block:
// the lookups, the input checks, the sigop count, the undo data and the
// writes into the view. WarmScripts adds script verification with a
// ValidationCache that is warm after the first iteration, which is the state
// a block reconstructed from the mempool finds at the tip; it runs the core
// in its just_check mode, the one that keeps cache entries on a hit, so the
// cache stays warm across iterations. That mode also leaves the block's
// checked flag unset, so CheckBlock()'s per-transaction work runs again on
// every iteration, which NoScripts does once: WarmScripts' difference from it
// is the scripts and those checks.
//
// The Tip pair adds what the tip does around the core, without scripts, as one
// round trip: the block connected and made the tip's state, then disconnected
// into a child of that state and flushed back, as a reorganisation does, so
// that the next iteration finds the prevouts again. TipStore takes the tip's
// steps in the coins store: the block opened in a BlockApplyView, its inputs
// prefetched, the core, and Confirm(), which applies the block's changes to
// the store and confirms them. TipOverlay takes the overlay's: a CoinsViewOverlay
// over a cache, the same prefetch, the core and a flush into the cache. Both
// prefetch with DEFAULT_PREVOUTFETCH_THREADS threads (the kernel's default;
// bitcoind's is one per core). Both pay for a disconnect, each into its own
// structure, so their difference is what the store costs at the tip for
// the connect and the disconnect together.

#include <bench/bench.h>
#include <bench/data/block413567.raw.h>
#include <bench/data/block413567_undo.raw.h>
#include <chain.h>
#include <coins.h>
#include <coinsstore.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <kernel/chainstatemanager_opts.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/threadpool.h>
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
        AddPrevouts(seeded);
    }

    //! The coins the block spends, but those it creates itself: the block puts those there, and a disconnect
    //! would find one seeded under the output it takes back.
    void AddPrevouts(CCoinsViewCache& view) const
    {
        for (size_t i{1}; i < block.vtx.size(); ++i) {
            const CTransaction& tx{*block.vtx[i]};
            const CTxUndo& tx_undo{undo.vtxundo[i - 1]};
            Assert(tx_undo.vprevout.size() == tx.vin.size());
            for (size_t j{0}; j < tx.vin.size(); ++j) {
                if (tx_undo.vprevout[j].nHeight == BLOCK_HEIGHT) continue;
                view.AddCoin(tx.vin[j].prevout, Coin{tx_undo.vprevout[j]}, /*possible_overwrite=*/false);
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
            .block_hash = block_hash,
            .prev_block_hash = block.hashPrevBlock,
            .height = BLOCK_HEIGHT,
            .prev_median_time_past = chain[BLOCK_HEIGHT - 1].GetMedianTimePast(),
            // Above BIP34's height, on the chain with the block the parameters name there.
            .enforce_bip30 = false,
            .median_time_past_at = [this](int height) { return chain[height].GetMedianTimePast(); },
            .defer_missing_inputs = false,
        };
    }
};

void ConnectMainnetBlock(benchmark::Bench& bench, bool check_scripts)
{
    MainnetBlock data;
    const BlockValidationContext ctx{data.Context(check_scripts)};
    bench.unit("block").run([&] {
        CCoinsViewCache view{&data.seeded};
        BlockValidationState state;
        ConnectBlockResult result;
        const bool ok{ConnectBlock(data.block, ctx, view, /*check_queue=*/nullptr, data.validation_cache,
                                   /*just_check=*/check_scripts, state, result)};
        assert(ok);
        ankerl::nanobench::doNotOptimizeAway(result.num_inputs);
    });
}

//! Take the block back out of `tip` from its undo data, as DisconnectTip() does: into a child, flushed.
void Disconnect(const MainnetBlock& data, CBlockUndo&& undo, CCoinsViewCache& tip)
{
    CCoinsViewCache view{&tip};
    const DisconnectResult res{DisconnectBlock(data.block, std::move(undo), data.block_hash, BLOCK_HEIGHT, view)};
    assert(res == DISCONNECT_OK);
    view.Flush(/*reallocate_cache=*/false);
}

util::NotNullSharedPtr<ThreadPool> FetchPool()
{
    util::NotNull pool{std::make_shared<ThreadPool>("bench")};
    pool->Start(DEFAULT_PREVOUTFETCH_THREADS);
    return pool;
}

void ConnectBlockMainnetTipStore(benchmark::Bench& bench)
{
    MainnetBlock data;
    const BlockValidationContext ctx{data.Context(/*check_scripts=*/false)};
    const auto pool{FetchPool()};
    CoinsStore store{&CoinsViewEmpty::Get()};
    store.SetBestBlock(data.block.hashPrevBlock, BLOCK_HEIGHT - 1);
    {
        CCoinsViewCache seed{&store};
        data.AddPrevouts(seed);
        seed.Flush(/*reallocate_cache=*/false);
    }
    BlockApplyView view{store, pool};
    bench.unit("block").run([&] {
        view.SetBestBlock(data.block.hashPrevBlock);
        view.BeginBlock(BLOCK_HEIGHT);
        BlockValidationState state;
        ConnectBlockResult result;
        {
            const auto fetch{view.StartFetching(data.block)};
            const bool ok{ConnectBlock(data.block, ctx, view, /*check_queue=*/nullptr, data.validation_cache,
                                       /*just_check=*/false, state, result)};
            assert(ok);
            view.Confirm();
        }
        Disconnect(data, std::move(result.undo), store);
        store.SetTierHeight(BLOCK_HEIGHT - 1);
    });
}

void ConnectBlockMainnetTipOverlay(benchmark::Bench& bench)
{
    MainnetBlock data;
    const BlockValidationContext ctx{data.Context(/*check_scripts=*/false)};
    const auto pool{FetchPool()};
    CCoinsViewCache& tip{data.seeded};
    bench.unit("block").run([&] {
        BlockValidationState state;
        ConnectBlockResult result;
        {
            CoinsViewOverlay view{&tip, pool};
            const auto fetch{view.StartFetching(data.block)};
            const bool ok{ConnectBlock(data.block, ctx, view, /*check_queue=*/nullptr, data.validation_cache,
                                       /*just_check=*/false, state, result)};
            assert(ok);
            view.Flush(/*reallocate_cache=*/false);
        }
        Disconnect(data, std::move(result.undo), tip);
    });
}

void ConnectBlockMainnetNoScripts(benchmark::Bench& bench) { ConnectMainnetBlock(bench, /*check_scripts=*/false); }
void ConnectBlockMainnetWarmScripts(benchmark::Bench& bench) { ConnectMainnetBlock(bench, /*check_scripts=*/true); }

} // namespace

BENCHMARK(ConnectBlockMainnetNoScripts);
BENCHMARK(ConnectBlockMainnetWarmScripts);
BENCHMARK(ConnectBlockMainnetTipStore);
BENCHMARK(ConnectBlockMainnetTipOverlay);
