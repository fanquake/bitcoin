// Copyright (c) 2016-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data/block413567.raw.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <txhashing.h>
#include <util/check.h>
#include <util/threadpool.h>
#include <validation.h>

#include <memory>
#include <span>
#include <vector>

// These are the two major time-sinks which happen after we have fully received
// a block off the wire, but before we can relay the block on to peers using
// compact block relay.

static void DeserializeBlockTest(benchmark::Bench& bench)
{
    const auto block_data{benchmark::data::block413567};
    bench.unit("block").run([&] {
        CBlock block;
        SpanReader{block_data} >> TX_WITH_WITNESS(block);
        assert(block.vtx.size() == 1557);
    });
}

//! The parse alone, into mutable transactions: the deserialisation cost without any hashing.
static void DeserializeBlockParseOnlyTest(benchmark::Bench& bench)
{
    const auto block_data{benchmark::data::block413567};
    bench.unit("block").run([&] {
        SpanReader reader{block_data};
        CBlockHeader header;
        reader >> header;
        const size_t count{ReadCompactSize(reader)};
        std::vector<CMutableTransaction> txs(count);
        for (auto& tx : txs) UnserializeTransaction(tx, reader, TX_WITH_WITNESS);
        assert(txs.size() == 1557);
    });
}

//! The block deserialiser of txhashing.h, hashing from the bytes on the reading thread.
static void DeserializeBlockHashedTest(benchmark::Bench& bench)
{
    const auto block_data{benchmark::data::block413567};
    bench.unit("block").run([&] {
        CBlock block;
        DeserializeBlock(std::as_bytes(std::span{block_data}), block, nullptr);
        assert(block.vtx.size() == 1557);
    });
}

//! The same with the hashing shared over four worker threads.
static void DeserializeBlockHashedParallelTest(benchmark::Bench& bench)
{
    const auto block_data{benchmark::data::block413567};
    ThreadPool pool{"bench"};
    pool.Start(4);
    bench.unit("block").run([&] {
        CBlock block;
        DeserializeBlock(std::as_bytes(std::span{block_data}), block, &pool);
        assert(block.vtx.size() == 1557);
    });
}

static void CheckBlockTest(benchmark::Bench& bench)
{
    const auto& chain_params{CChainParams::Main()};
    const auto block_data{benchmark::data::block413567};

    CBlock block;
    bench.unit("block")
        .setup([&] {
            block = CBlock{};
            SpanReader{block_data} >> TX_WITH_WITNESS(block);
            assert(block.vtx.size() == 1557);
        })
        .run([&] {
            BlockValidationState validationState;
            const bool checked{CheckBlock(block, validationState, chain_params->GetConsensus())};
            assert(checked);
        });
}

BENCHMARK(DeserializeBlockTest);
BENCHMARK(DeserializeBlockParseOnlyTest);
BENCHMARK(DeserializeBlockHashedTest);
BENCHMARK(DeserializeBlockHashedParallelTest);
BENCHMARK(CheckBlockTest);
