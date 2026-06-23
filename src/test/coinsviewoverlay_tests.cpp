// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <txdb.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/hasher.h>

#include <test/util/framework.h>

#include <cstdint>
#include <cstring>
#include <ranges>

TEST_SUITE_BEGIN(coinsviewoverlay_tests)

namespace {

CBlock CreateBlock() noexcept
{
    static constexpr auto NUM_TXS{100};
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back();
    block.vtx.push_back(MakeTransactionRef(coinbase));

    for (const auto i : std::views::iota(1, NUM_TXS)) {
        CMutableTransaction tx;
        Txid txid{Txid::FromUint256(uint256(i))};
        tx.vin.emplace_back(txid, 0);
        block.vtx.push_back(MakeTransactionRef(tx));
    }

    return block;
}

void PopulateView(const CBlock& block, CCoinsView& view, bool spent = false)
{
    CCoinsViewCache cache{&view};
    cache.SetBestBlock(uint256::ONE);

    for (const auto& tx : block.vtx | std::views::drop(1)) {
        for (const auto& in : tx->vin) {
            Coin coin{};
            if (!spent) coin.out.nValue = 1;
            cache.EmplaceCoinInternalDANGER(COutPoint{in.prevout}, std::move(coin));
        }
    }

    cache.Flush();
}

void CheckCache(const CBlock& block, const CCoinsViewCache& cache)
{
    uint32_t counter{0};

    for (const auto& tx : block.vtx) {
        if (tx->IsCoinBase()) {
            CHECK(!cache.HaveCoinInCache(tx->vin[0].prevout));
        } else {
            for (const auto& in : tx->vin) {
                const auto& outpoint{in.prevout};
                const auto& first{cache.AccessCoin(outpoint)};
                const auto& second{cache.AccessCoin(outpoint)};
                CHECK(&first == &second);
                ++counter;
                CHECK(cache.HaveCoinInCache(outpoint));
            }
        }
    }
    CHECK(cache.GetCacheSize() == counter);
}

} // namespace

TEST_CASE(fetch_inputs_from_db)
{
    const auto block{CreateBlock()};
    CCoinsViewDB db{{.path = "", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    PopulateView(block, db);
    CCoinsViewCache main_cache{&db};
    CoinsViewOverlay view{&main_cache};
    const auto& outpoint{block.vtx[1]->vin[0].prevout};

    CHECK(view.HaveCoin(outpoint));
    CHECK(view.GetCoin(outpoint).has_value());
    CHECK(!main_cache.HaveCoinInCache(outpoint));

    CheckCache(block, view);
    // Check that no coins have been moved up to main cache from db
    for (const auto& tx : block.vtx) {
        for (const auto& in : tx->vin) {
            CHECK(!main_cache.HaveCoinInCache(in.prevout));
        }
    }

    view.SetBestBlock(uint256::ONE);
    CHECK(view.SpendCoin(outpoint));
    view.Flush();
    CHECK(!main_cache.PeekCoin(outpoint).has_value());
}

TEST_CASE(fetch_inputs_from_cache)
{
    const auto block{CreateBlock()};
    CCoinsViewDB db{{.path = "", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CCoinsViewCache main_cache{&db};
    PopulateView(block, main_cache);
    CoinsViewOverlay view{&main_cache};
    CheckCache(block, view);

    const auto& outpoint{block.vtx[1]->vin[0].prevout};
    view.SetBestBlock(uint256::ONE);
    CHECK(view.SpendCoin(outpoint));
    view.Flush();
    CHECK(!main_cache.PeekCoin(outpoint).has_value());
}

// Test for the case where a block spends coins that are spent in the cache, but
// the spentness has not been flushed to the db.
TEST_CASE(fetch_no_double_spend)
{
    const auto block{CreateBlock()};
    CCoinsViewDB db{{.path = "", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    PopulateView(block, db);
    CCoinsViewCache main_cache{&db};
    // Add all inputs as spent already in cache
    PopulateView(block, main_cache, /*spent=*/true);
    CoinsViewOverlay view{&main_cache};
    for (const auto& tx : block.vtx) {
        for (const auto& in : tx->vin) {
            const auto& c{view.AccessCoin(in.prevout)};
            CHECK(c.IsSpent());
            CHECK(!view.HaveCoin(in.prevout));
            CHECK(!view.GetCoin(in.prevout));
        }
    }
    // Coins are not added to the view, even though they exist unspent in the parent db
    CHECK(view.GetCacheSize() == 0U);
}

TEST_CASE(fetch_no_inputs)
{
    const auto block{CreateBlock()};
    CCoinsViewDB db{{.path = "", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CCoinsViewCache main_cache{&db};
    CoinsViewOverlay view{&main_cache};
    for (const auto& tx : block.vtx) {
        for (const auto& in : tx->vin) {
            const auto& c{view.AccessCoin(in.prevout)};
            CHECK(c.IsSpent());
            CHECK(!view.HaveCoin(in.prevout));
            CHECK(!view.GetCoin(in.prevout));
        }
    }
    CHECK(view.GetCacheSize() == 0U);
}

TEST_SUITE_END()

