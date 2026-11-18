// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_memusage.h>
#include <node/blockcache.h>
#include <node/chainstatemanager_args.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <util/byte_ledger.h>
#include <util/byte_units.h>

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
    util::ByteAccount account;
    BlockCache cache{3 * one + one / 2, account}; // room for three

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
    util::ByteAccount tiny_account;
    BlockCache tiny{one / 2, tiny_account};
    tiny.Insert(10, MakeBlock(10), /*tip_height=*/1);
    BOOST_CHECK_EQUAL(tiny.Count(), 0U);
}

BOOST_AUTO_TEST_CASE(disabled)
{
    util::ByteAccount account;
    BlockCache cache{0, account};
    cache.Insert(5, MakeBlock(5), /*tip_height=*/1);
    BOOST_CHECK_EQUAL(cache.Count(), 0U);
    BOOST_CHECK(!cache.Take(5, MakeBlock(5)->GetHash()));
}

BOOST_AUTO_TEST_CASE(take_reports_measured_bytes)
{
    // A block whose witness the window's count-based estimate would not see: one input, 100 kB of witness.
    auto heavy{std::make_shared<CBlock>()};
    CMutableTransaction tx;
    tx.vin.emplace_back();
    tx.vin[0].scriptWitness.stack.emplace_back(100'000, 0x01);
    heavy->vtx.push_back(MakeTransactionRef(std::move(tx)));
    const std::shared_ptr<const CBlock> block{heavy};
    BOOST_CHECK_GT(BlockCache::Bytes(*block), 100'000U);

    util::ByteAccount account;
    BlockCache cache{size_t{1} << 20, account};
    cache.Insert(5, block, /*tip_height=*/1);
    cache.Insert(6, MakeBlock(6), /*tip_height=*/1, /*bytes=*/12'345);
    size_t bytes{0};
    BOOST_CHECK(cache.Take(5, block->GetHash(), &bytes));
    BOOST_CHECK_EQUAL(bytes, BlockCache::Bytes(*block));
    // A size the caller measured at insertion is the one reported.
    BOOST_CHECK(cache.Take(6, MakeBlock(6)->GetHash(), &bytes));
    BOOST_CHECK_EQUAL(bytes, 12'345U);
    // A block not found leaves the output alone.
    bytes = 7;
    BOOST_CHECK(!cache.Take(6, MakeBlock(6)->GetHash(), &bytes));
    BOOST_CHECK_EQUAL(bytes, 7U);
}

//! With the budget used up by blocks the tip has not passed, a block above every one kept is neither measured
//! nor kept, even one that would fit; once a block has left, such a block is kept again. A copy dropped because
//! another one was connected leaves without counting as a hit.
BOOST_AUTO_TEST_CASE(full_cache_skips_a_higher_block)
{
    const size_t one{sizeof(CBlock) + RecursiveDynamicUsage(*MakeBlock(1))};
    util::ByteAccount account;
    BlockCache cache{3 * one + one / 2, account}; // room for three
    for (const int height : {2, 3, 4, 5}) cache.Insert(height, MakeBlock(height), /*tip_height=*/1);
    BOOST_CHECK_EQUAL(cache.Count(), 3U); // 5 went at once: the budget is used up

    auto small{std::make_shared<CBlock>()};
    small->nNonce = 9;
    BOOST_REQUIRE(BlockCache::Bytes(*small) < one / 2); // it would fit
    cache.Insert(9, small, /*tip_height=*/1);
    BOOST_CHECK_EQUAL(cache.Count(), 3U);
    // A known size that does not fit is skipped as well.
    cache.Insert(7, MakeBlock(7), /*tip_height=*/1, one);
    BOOST_CHECK_EQUAL(cache.Count(), 3U);

    cache.Drop(4, MakeBlock(4)->GetHash());
    BOOST_CHECK_EQUAL(cache.Count(), 2U);
    cache.Insert(9, small, /*tip_height=*/1);
    BOOST_CHECK_EQUAL(cache.Count(), 3U);
    BOOST_CHECK(cache.Take(9, small->GetHash()));
}

BOOST_AUTO_TEST_CASE(the_block_cache_counts_in_its_account)
{
    util::ByteLedger ledger;
    util::ByteAccount& account{ledger[util::ByteAccountId::BlockCache]};
    const size_t one{BlockCache::Bytes(*MakeBlock(1))};
    BlockCache cache{3 * one + one / 2, account}; // room for three
    BOOST_CHECK_EQUAL(account.Limit(), 3 * one + one / 2);

    for (const int height : {5, 3, 4, 6, 2}) cache.Insert(height, MakeBlock(height), /*tip_height=*/1);
    // Each insertion over the limit dropped the highest block.
    BOOST_CHECK_EQUAL(cache.Count(), 3U);
    BOOST_CHECK_EQUAL(account.Bytes(), 3 * one);
    BOOST_CHECK(account.Admits());
    BOOST_CHECK(!account.Admits(one));

    BOOST_CHECK(cache.Take(2, MakeBlock(2)->GetHash()));
    BOOST_CHECK_EQUAL(account.Bytes(), 2 * one);
    cache.Drop(3, MakeBlock(3)->GetHash());
    BOOST_CHECK_EQUAL(account.Bytes(), one);
    // The tip passes the last block: it leaves at the next insertion, which the tip has passed as well.
    cache.Insert(7, MakeBlock(7), /*tip_height=*/4);
    BOOST_CHECK_EQUAL(account.Bytes(), one);
    BOOST_CHECK(cache.Take(7, MakeBlock(7)->GetHash()));
    BOOST_CHECK_EQUAL(account.Bytes(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
