// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_memusage.h>
#include <node/blockcache.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

using node::BlockCache;

BOOST_FIXTURE_TEST_SUITE(blockcache_tests, BasicTestingSetup)

namespace {

//! A block of about a kilobyte whose hash depends on `height` only.
std::shared_ptr<const CBlock> MakeBlock(int height)
{
    auto block{std::make_shared<CBlock>()};
    block->nNonce = height;
    CMutableTransaction tx;
    tx.vin.emplace_back();
    const std::vector<unsigned char> bytes(1000, 0x51);
    tx.vin[0].scriptSig = CScript(bytes.begin(), bytes.end());
    block->vtx.push_back(MakeTransactionRef(std::move(tx)));
    return block;
}

} // namespace

BOOST_AUTO_TEST_CASE(budget_and_order)
{
    const size_t one{sizeof(CBlock) + RecursiveDynamicUsage(*MakeBlock(1))};
    BlockCache cache{3 * one + one / 2}; // room for three

    for (const int height : {5, 3, 4, 6, 2}) cache.Insert(height, MakeBlock(height), /*tip_height=*/1);
    // Each insertion over budget dropped the highest block: 2, 3 and 4 remain.
    BOOST_CHECK_EQUAL(cache.Count(), 3U);
    BOOST_CHECK_EQUAL(cache.Bytes(), 3 * one);
    BOOST_CHECK(!cache.Take(6, MakeBlock(6)->GetHash()));
    BOOST_CHECK(!cache.Take(5, MakeBlock(5)->GetHash()));
    // A wrong height or hash does not find it.
    BOOST_CHECK(!cache.Take(3, MakeBlock(2)->GetHash()));
    BOOST_CHECK(!cache.Take(1, MakeBlock(2)->GetHash()));

    const auto two{cache.Take(2, MakeBlock(2)->GetHash())};
    BOOST_REQUIRE(two);
    BOOST_CHECK_EQUAL(two->nNonce, 2U);
    BOOST_CHECK(!cache.Take(2, two->GetHash())); // taken means gone
    BOOST_CHECK_EQUAL(cache.Count(), 2U);
    const auto [hits, misses]{cache.HitsAndMisses()};
    BOOST_CHECK_EQUAL(hits, 1U);
    BOOST_CHECK_EQUAL(misses, 5U);

    // The same block twice is kept once.
    cache.Insert(7, MakeBlock(7), /*tip_height=*/1);
    cache.Insert(7, MakeBlock(7), /*tip_height=*/1);
    BOOST_CHECK_EQUAL(cache.Count(), 3U);

    // Blocks the tip has passed are dropped on the next insertion.
    cache.Insert(9, MakeBlock(9), /*tip_height=*/4);
    BOOST_CHECK_EQUAL(cache.Count(), 2U);
    BOOST_CHECK(!cache.Take(3, MakeBlock(3)->GetHash()));
    BOOST_CHECK(cache.Take(7, MakeBlock(7)->GetHash()));
    BOOST_CHECK(cache.Take(9, MakeBlock(9)->GetHash()));
    BOOST_CHECK_EQUAL(cache.Bytes(), 0U);

    // A block at or below the tip, and a block larger than the budget, are not kept.
    cache.Insert(4, MakeBlock(4), /*tip_height=*/4);
    BOOST_CHECK_EQUAL(cache.Count(), 0U);
    BlockCache tiny{one / 2};
    tiny.Insert(10, MakeBlock(10), /*tip_height=*/1);
    BOOST_CHECK_EQUAL(tiny.Count(), 0U);
}

BOOST_AUTO_TEST_CASE(disabled)
{
    BlockCache cache{0};
    cache.Insert(5, MakeBlock(5), /*tip_height=*/1);
    BOOST_CHECK_EQUAL(cache.Count(), 0U);
    BOOST_CHECK(!cache.Take(5, MakeBlock(5)->GetHash()));
    const auto [hits, misses]{cache.HitsAndMisses()};
    BOOST_CHECK_EQUAL(hits, 0U);
    BOOST_CHECK_EQUAL(misses, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
