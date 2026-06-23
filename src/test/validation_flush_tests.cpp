// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sync.h>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(validation_flush_tests)

//! Verify that Chainstate::GetCoinsCacheSizeState() switches from OK→LARGE→CRITICAL
//! at the expected utilization thresholds, first with *no* mempool head-room,
//! then with additional mempool head-room.
FIXTURE_TEST_CASE(getcoinscachesizestate, TestingSetup)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};

    LOCK(::cs_main);
    CCoinsViewCache& view{chainstate.CoinsTip()};

    // Sanity: an empty cache should be ≲ 1 chunk (~ 256 KiB).
    CHECK(view.DynamicMemoryUsage() / (256 * 1024.0) < 1.1);

    constexpr size_t MAX_COINS_BYTES{8_MiB};
    constexpr size_t MAX_MEMPOOL_BYTES{4_MiB};
    constexpr size_t MAX_ATTEMPTS{50'000};

    // Run the same growth-path twice: first with 0 head-room, then with extra head-room
    for (size_t max_mempool_size_bytes : {size_t{0}, MAX_MEMPOOL_BYTES}) {
        const int64_t full_cap{int64_t(MAX_COINS_BYTES + max_mempool_size_bytes)};
        const int64_t large_cap{LargeCoinsCacheThreshold(full_cap)};

        // OK → LARGE
        auto state{chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, max_mempool_size_bytes)};
        for (size_t i{0}; i < MAX_ATTEMPTS && int64_t(view.DynamicMemoryUsage()) <= large_cap; ++i) {
            CHECK(state == CoinsCacheSizeState::OK);
            AddTestCoin(m_rng, view);
            state = chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, max_mempool_size_bytes);
        }

        // LARGE → CRITICAL
        for (size_t i{0}; i < MAX_ATTEMPTS && int64_t(view.DynamicMemoryUsage()) <= full_cap; ++i) {
            CHECK(state == CoinsCacheSizeState::LARGE);
            AddTestCoin(m_rng, view);
            state = chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, max_mempool_size_bytes);
        }
        CHECK(state == CoinsCacheSizeState::CRITICAL);
    }

    // Default thresholds (no explicit limits) permit many more coins.
    for (int i{0}; i < 1'000; ++i) {
        AddTestCoin(m_rng, view);
        CHECK(chainstate.GetCoinsCacheSizeState() == CoinsCacheSizeState::OK);
    }

    // CRITICAL → OK via Flush
    CHECK(chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, /*max_mempool_size_bytes=*/0) == CoinsCacheSizeState::CRITICAL);
    view.SetBestBlock(m_rng.rand256());
    view.Flush();
    CHECK(chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, /*max_mempool_size_bytes=*/0) == CoinsCacheSizeState::OK);
}

TEST_SUITE_END()
