// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coinsstore.h>
#include <script/script.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <util/byte_units.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <optional>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(coinsstore_tests, BasicTestingSetup)

namespace {

COutPoint RandomOutpoint(FastRandomContext& rng, uint32_t txids)
{
    // A small txid space so that the same outpoints are revisited.
    uint256 h;
    h.data()[0] = static_cast<unsigned char>(rng.randrange(txids));
    return COutPoint{Txid::FromUint256(h), static_cast<uint32_t>(rng.randrange(3))};
}

Coin RandomCoin(FastRandomContext& rng, int height)
{
    CTxOut out{static_cast<CAmount>(rng.randrange(1000) + 1), CScript() << std::vector<unsigned char>(rng.randbytes(4))};
    return Coin{std::move(out), height, /*fCoinBaseIn=*/false};
}

//! The visible state of a view over a universe of outpoints.
std::map<COutPoint, std::optional<Coin>> Visible(const CCoinsView& view, const std::vector<COutPoint>& universe)
{
    std::map<COutPoint, std::optional<Coin>> out;
    for (const COutPoint& op : universe) out[op] = view.PeekCoin(op);
    return out;
}

bool Same(const std::optional<Coin>& a, const std::optional<Coin>& b)
{
    if (!a && !b) return true;
    if (!a || !b) return false;
    return a->out == b->out && a->nHeight == b->nHeight && a->fCoinBase == b->fCoinBase;
}

void CheckSameVisible(const CCoinsView& a, const CCoinsView& b, const std::vector<COutPoint>& universe)
{
    const auto va{Visible(a, universe)};
    const auto vb{Visible(b, universe)};
    for (const COutPoint& op : universe) {
        BOOST_CHECK_MESSAGE(Same(va.at(op), vb.at(op)), "mismatch at " << op.ToString());
    }
}

std::vector<COutPoint> Universe(uint32_t txids)
{
    std::vector<COutPoint> out;
    for (uint32_t t{0}; t < txids; ++t) {
        uint256 h;
        h.data()[0] = static_cast<unsigned char>(t);
        for (uint32_t n{0}; n < 3; ++n) out.emplace_back(Txid::FromUint256(h), n);
    }
    return out;
}

} // namespace

//! The store, driven as a cache would be (adds, spends, child flushes, full flushes), shows the same coins
//! as a CCoinsViewCache over an identical database at every step.
BOOST_AUTO_TEST_CASE(same_as_cache_under_random_operations)
{
    constexpr uint32_t TXIDS{24};
    const auto universe{Universe(TXIDS)};
    CCoinsViewDB db_cache{{.path = "cache", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CCoinsViewDB db_store{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CCoinsViewCache cache{&db_cache};
    CoinsStore store{&db_store, /*shards=*/8, /*deterministic=*/true};
    const uint256 genesis{m_rng.rand256()};
    cache.SetBestBlock(genesis);
    store.SetBestBlock(genesis, 0);

    for (int step{1}; step < 400; ++step) {
        const uint256 block_hash{m_rng.rand256()};
        store.SetChangeHeight(step);
        // Some operations straight on the tier, some through a child cache as a block connection does.
        if (m_rng.randbool()) {
            for (int i{0}; i < 6; ++i) {
                const COutPoint op{RandomOutpoint(m_rng, TXIDS)};
                if (m_rng.randrange(3) == 0) {
                    // SpendCoin() answers true for any entry it holds, spent or not, in both; the store keeps
                    // spent entries of blocks above a run's bound where the cache dropped them at its full
                    // write, so only the answer for a visible coin is comparable.
                    const bool had{cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(had, store.HaveCoin(op));
                    const bool a{cache.SpendCoin(op)};
                    const bool b{store.SpendCoin(op)};
                    if (had) BOOST_CHECK(a && b);
                } else {
                    Coin coin{RandomCoin(m_rng, step)};
                    Coin copy{coin};
                    const bool overwrite{cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(overwrite, store.HaveCoin(op));
                    cache.AddCoin(op, std::move(coin), overwrite);
                    store.AddCoin(op, std::move(copy), overwrite);
                }
            }
            cache.SetBestBlock(block_hash);
            store.SetBestBlock(block_hash);
        } else {
            CCoinsViewCache child_cache{&cache};
            CCoinsViewCache child_store{&store};
            for (int i{0}; i < 6; ++i) {
                const COutPoint op{RandomOutpoint(m_rng, TXIDS)};
                if (m_rng.randrange(3) == 0) {
                    const bool had{child_cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(had, child_store.HaveCoin(op));
                    const bool a{child_cache.SpendCoin(op)};
                    const bool b{child_store.SpendCoin(op)};
                    if (had) BOOST_CHECK(a && b);
                } else {
                    Coin coin{RandomCoin(m_rng, step)};
                    Coin copy{coin};
                    const bool overwrite{child_cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(overwrite, child_store.HaveCoin(op));
                    child_cache.AddCoin(op, std::move(coin), overwrite);
                    child_store.AddCoin(op, std::move(copy), overwrite);
                }
            }
            child_cache.SetBestBlock(block_hash);
            child_store.SetBestBlock(block_hash);
            child_cache.Flush();
            child_store.Flush();
        }
        CheckSameVisible(cache, store, universe);
        BOOST_CHECK(cache.GetBestBlock() == store.GetBestBlock());

        // Now and then write out: the cache in full, the store either in full or as a lag run of its
        // oldest changes; afterwards the databases must agree on everything the store has written and
        // the views must still agree on everything.
        if (step % 25 == 0) {
            cache.Sync();
            if (m_rng.randbool()) {
                store.Sync();
                CheckSameVisible(db_cache, db_store, universe);
                BOOST_CHECK(db_store.GetBestBlock() == block_hash);
                BOOST_CHECK(db_store.GetHeadBlocks().empty());
            } else {
                const int bound{step - 1 - static_cast<int>(m_rng.randrange(19))};
                auto run{store.PrepareWriteOut(bound, step, block_hash, /*final=*/false)};
                if (run) {
                    BOOST_CHECK(run->settled_height >= bound && run->settled_height <= step);
                    run->settled_head = run->settled_height == step ? block_hash : uint256{static_cast<uint8_t>(1 + run->settled_height % 255)}; // any label but the null hash
                    // Changes made while the run is in flight stay dirty.
                    const COutPoint late{RandomOutpoint(m_rng, TXIDS)};
                    const bool late_spent{store.HaveCoin(late)};
                    store.SpendCoin(late);
                    cache.SpendCoin(late);
                    store.WriteRun(run->writes, run->new_head, run->old_head, run->final);
                    store.CommitWriteOut(*run);
                    BOOST_CHECK(store.WrittenHeight() == run->settled_height);
                    BOOST_CHECK(store.WrittenHead() == run->settled_head);
                    const auto heads{db_store.GetHeadBlocks()};
                    BOOST_REQUIRE_EQUAL(heads.size(), 2U);
                    BOOST_CHECK(heads[0] == block_hash);
                    BOOST_CHECK(db_store.GetBestBlock().IsNull());
                    // Nothing changed at or below the bound is still dirty, unless it changed again since.
                    const auto oldest{store.OldestDirtyHeight()};
                    BOOST_CHECK(!oldest || *oldest > bound || late_spent);
                    cache.Sync();
                }
            }
            CheckSameVisible(cache, store, universe);
            store.SanityCheck();
        }
    }
    // A final drain leaves both databases identical and consistent.
    cache.Flush();
    store.Flush();
    CheckSameVisible(db_cache, db_store, universe);
    BOOST_CHECK(db_store.GetHeadBlocks().empty());
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
}

/**
 * The settled head after a run is the block whose state the database holds, not the bound: a reorg's
 * inverse changes are tagged with the fork height so that any run carries them all, and the run that
 * does so moves the database to the tier's head. A coin a disconnect restores that was created above
 * the settled height stays out of the database until its creating block is written.
 */
BOOST_AUTO_TEST_CASE(settled_head_follows_the_state_not_the_bound)
{
    CCoinsViewDB db{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CoinsStore store{&db, 4, true};
    const auto hash{[](int h) { return uint256{static_cast<uint8_t>(h)}; }};
    const auto outpoint{[](int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + n)}), 0}; }};

    // Blocks 300..307 connected and drained: the database holds the state at 307.
    for (int h{300}; h <= 307; ++h) {
        store.SetBestBlock(hash(h), h);
        store.AddCoin(outpoint(h), Coin{CTxOut{CAmount{h}, CScript() << OP_TRUE}, h, false}, false);
    }
    store.SetChangeHeight(306);
    store.SpendCoin(outpoint(300)); // spent in 306
    store.SetChangeHeight(307);
    store.SpendCoin(outpoint(301)); // spent in 307
    store.Sync();
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 307);
    BOOST_CHECK(store.WrittenHead() == hash(307));

    // Disconnect 307 towards a fork at 301: its inverse changes are tagged 301.
    store.SetChangeHeight(301);
    store.SpendCoin(outpoint(307));                                                              // 307's coinbase goes
    store.AddCoin(outpoint(301), Coin{CTxOut{CAmount{301}, CScript() << OP_TRUE}, 301, false}, false); // 307's spend undone
    store.SetBestBlock(hash(306));
    store.SetTierHeight(306);
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 301);

    // A run bounded at 301 carries both and leaves nothing dirty: the database then holds 306's state,
    // and that, not 301, is what the next marker must name as its old head.
    auto run{store.PrepareWriteOut(/*bound_height=*/301, /*head_height=*/306, hash(306), /*final=*/false)};
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 2U);
    BOOST_CHECK(run->old_head == hash(307));
    BOOST_CHECK_EQUAL(run->settled_height, 306);
    run->settled_head = hash(306);
    store.WriteRun(run->writes, run->new_head, run->old_head, run->final);
    store.CommitWriteOut(*run);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 306);
    BOOST_CHECK(store.WrittenHead() == hash(306));
    BOOST_CHECK(!store.OldestDirtyHeight());

    // Connect 307' and 308' on another branch, unwritten: 307' creates a coin that 308' spends, and 308'
    // also spends a coin the database has (created in 304).
    store.SetBestBlock(hash(57), 307);
    store.AddCoin(outpoint(57), Coin{CTxOut{CAmount{57}, CScript() << OP_TRUE}, 307, false}, false);
    store.SetBestBlock(hash(58), 308);
    store.AddCoin(outpoint(58), Coin{CTxOut{CAmount{58}, CScript() << OP_TRUE}, 308, false}, false);
    BOOST_CHECK(store.SpendCoin(outpoint(57)));
    BOOST_CHECK(store.SpendCoin(outpoint(304)));

    // Disconnect 308' towards a fork at 306. The restored coin from 304 is in the database and takes the
    // fork height; the restored coin from 307' is not, and keeps 307 so that no run below 307 writes it.
    store.SetChangeHeight(306);
    store.SpendCoin(outpoint(58));
    store.AddCoin(outpoint(304), Coin{CTxOut{CAmount{304}, CScript() << OP_TRUE}, 304, false}, false);
    store.AddCoin(outpoint(57), Coin{CTxOut{CAmount{57}, CScript() << OP_TRUE}, 307, false}, false);
    store.SetBestBlock(hash(57));
    store.SetTierHeight(307);
    auto run2{store.PrepareWriteOut(/*bound_height=*/306, /*head_height=*/307, hash(57), /*final=*/false)};
    BOOST_REQUIRE(run2);
    BOOST_REQUIRE_EQUAL(run2->writes.size(), 1U);
    BOOST_CHECK(run2->writes[0].outpoint == outpoint(304));
    BOOST_CHECK(run2->old_head == hash(306));
    // 307' remains dirty (its coinbase and the restored coin), so the database settles at 306 still.
    BOOST_CHECK_EQUAL(run2->settled_height, 306);
    run2->settled_head = hash(306);
    store.WriteRun(run2->writes, run2->new_head, run2->old_head, run2->final);
    store.CommitWriteOut(*run2);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 306);
    BOOST_CHECK(!db.HaveCoin(outpoint(57)));
    BOOST_CHECK(db.HaveCoin(outpoint(304)));

    // A final drain writes 307' in full.
    store.Sync();
    BOOST_CHECK(db.HaveCoin(outpoint(57)));
    BOOST_CHECK(!db.HaveCoin(outpoint(58)));
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 307);
    store.SanityCheck();
}

//! A coin created and spent inside the lag is never written, not even as an erase, and a spent entry
//! that was written is erased on the next run.
BOOST_AUTO_TEST_CASE(fresh_coins_leave_no_trace)
{
    CCoinsViewDB db{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    store.AddCoin(b, RandomCoin(m_rng, 1), false);
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(a)); // fresh: dropped outright
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 1U);
    auto run{store.PrepareWriteOut(2, 2, uint256{2}, false)};
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 1U); // only b
    BOOST_CHECK(run->writes[0].coin.has_value());
    run->settled_head = uint256{2};
    store.WriteRun(run->writes, run->new_head, run->old_head, false);
    store.CommitWriteOut(*run);
    BOOST_CHECK(db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    // b is in the database now; spending it must produce an erase even though it was fresh once.
    store.SetChangeHeight(3);
    BOOST_CHECK(store.SpendCoin(b));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    run = store.PrepareWriteOut(3, 3, uint256{3}, false);
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 1U);
    BOOST_CHECK(!run->writes[0].coin.has_value());
    run->settled_head = uint256{3};
    store.WriteRun(run->writes, run->new_head, run->old_head, false);
    store.CommitWriteOut(*run);
    BOOST_CHECK(!db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
}

//! A spend that lands between collection and commit keeps its entry dirty; a run only settles what it saw.
BOOST_AUTO_TEST_CASE(run_settles_only_unchanged_entries)
{
    CCoinsViewDB db{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    auto run{store.PrepareWriteOut(1, 1, uint256{1}, false)};
    BOOST_REQUIRE(run);
    run->settled_head = uint256{1};
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(a)); // no longer fresh: the run will put it in the database
    store.WriteRun(run->writes, run->new_head, run->old_head, false);
    store.CommitWriteOut(*run);
    BOOST_CHECK(db.PeekCoin(a).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U); // the erase is still owed
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 2);
    store.Sync();
    BOOST_CHECK(!db.PeekCoin(a).has_value());
}

//! Clean entries go first, and the bound for a byte target walks the height histogram from the oldest.
BOOST_AUTO_TEST_CASE(evict_clean_and_bound_for_bytes)
{
    CCoinsViewDB db{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    {
        CCoinsViewCache seed{&db};
        for (uint32_t i{0}; i < 50; ++i) seed.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}, RandomCoin(m_rng, 1), false);
        seed.SetBestBlock(uint256::ONE);
        seed.Flush();
    }
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    for (uint32_t i{0}; i < 50; ++i) BOOST_CHECK(store.HaveCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}));
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 50U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    for (int h{2}; h <= 11; ++h) {
        store.SetChangeHeight(h);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + h)}), 0}, RandomCoin(m_rng, h), false);
    }
    const size_t before{store.DynamicMemoryUsage()};
    store.EvictClean(0);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 10U);
    BOOST_CHECK(store.DynamicMemoryUsage() < before);
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 2);
    const size_t per_height{store.DirtyBytesAtOrBelow(2)};
    BOOST_CHECK(per_height > 0);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height, 11), 2);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 3, 11), 4);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 100, 11), 11);
    auto run{store.PrepareWriteOut(4, 11, uint256{11}, false)};
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 4); // height 5 stays dirty
    run->settled_head = uint256{4};
    store.WriteRun(run->writes, run->new_head, run->old_head, false);
    store.CommitWriteOut(*run);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 7U);
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 5);
    store.SanityCheck();
}

//! The dirty lists are kept by span of heights; sums, bounds and the settled height are exact to the height
//! all the same, inside a span and across spans.
BOOST_AUTO_TEST_CASE(bounds_are_exact_inside_and_across_dirty_spans)
{
    CCoinsViewDB db{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    {
        CCoinsViewCache seed{&db};
        seed.AddCoin(COutPoint{Txid::FromUint256(uint256{200}), 0}, RandomCoin(m_rng, 1), false);
        seed.SetBestBlock(uint256::ONE);
        seed.Flush();
    }
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const std::vector<int> heights{1000, 1020, 1030, 1040, 2100, 5000};
    for (size_t i{0}; i < heights.size(); ++i) {
        store.SetChangeHeight(heights[i]);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}, RandomCoin(m_rng, heights[i]), false);
    }
    store.SanityCheck();
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 1000);
    const size_t each{store.DirtyBytesAtOrBelow(1000)};
    BOOST_CHECK(each > 0);
    BOOST_CHECK_EQUAL(store.DirtyBytesAtOrBelow(999), 0U);
    BOOST_CHECK_EQUAL(store.DirtyBytesAtOrBelow(1029), each * 2);
    BOOST_CHECK_EQUAL(store.DirtyBytesAtOrBelow(1030), each * 3);
    BOOST_CHECK_EQUAL(store.DirtyBytesAtOrBelow(4999), each * 5);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 2, 5000), 1020);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 3, 5000), 1030);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 5, 5000), 2100);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 7, 5000), 5000);

    // A bound inside a span takes the heights at or below it and settles just below the next height, which
    // shares its list with what was written.
    auto run{store.PrepareWriteOut(/*bound_height=*/1030, /*head_height=*/5000, uint256{50}, /*final=*/false)};
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 1039);
    run->settled_head = uint256{39};
    store.WriteRun(run->writes, run->new_head, run->old_head, false);
    store.CommitWriteOut(*run);
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 1040);

    // A bound between two spans leaves the lists further up alone.
    run = store.PrepareWriteOut(/*bound_height=*/2000, /*head_height=*/5000, uint256{50}, /*final=*/false);
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 1U);
    BOOST_CHECK_EQUAL(run->settled_height, 2099);
    run->settled_head = uint256{99};
    store.WriteRun(run->writes, run->new_head, run->old_head, false);
    store.CommitWriteOut(*run);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 2U);
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 2100);
    store.SanityCheck();
}

//! Counts what is written through it: the runs, their changes, and what they come to as database batches.
class CountingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    Mutex m_mutex;
    size_t m_runs GUARDED_BY(m_mutex){0};
    size_t m_changes GUARDED_BY(m_mutex){0};
    uint64_t m_bytes GUARDED_BY(m_mutex){0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        {
            LOCK(m_mutex);
            ++m_runs;
            m_changes += writes.size();
            for (const CoinWrite& write : writes) m_bytes += write.coin ? 45 + write.coin->out.scriptPubKey.size() : 38;
        }
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A flush fills its batches across the shards up to the batch size, a batch not being a shard's worth of
//! changes, with several workers writing; the database then holds every change and the tier is empty.
BOOST_AUTO_TEST_CASE(flush_fills_batches_across_shards)
{
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CountingBase base{&db};
    CoinsStore store{&base, 16, true};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, std::optional<Coin>> expected;
    std::vector<COutPoint> first;
    for (int i{0}; i < 2000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        Coin coin{RandomCoin(m_rng, 1)};
        expected[outpoint] = coin;
        first.push_back(outpoint);
        store.AddCoin(outpoint, std::move(coin), false);
    }
    store.Sync(); // in the database, so that spending them owes an erase
    store.SetBestBlock(uint256{2}, 2);
    for (size_t i{0}; i < first.size(); i += 4) {
        BOOST_REQUIRE(store.SpendCoin(first[i]));
        expected[first[i]] = std::nullopt;
    }
    for (int i{0}; i < 2000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        Coin coin{RandomCoin(m_rng, 2)};
        expected[outpoint] = coin;
        store.AddCoin(outpoint, std::move(coin), false);
    }
    WITH_LOCK(base.m_mutex, base.m_runs = 0; base.m_changes = 0; base.m_bytes = 0);
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256{2});
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    BOOST_CHECK_EQUAL(base.m_changes, 2500U);
    // Full batches, then what each of the workers had left (eight at most), and the run that closes the marker: far
    // fewer and far larger than a run for each of the 16 shards' share would be at this batch size.
    BOOST_CHECK_GE(base.m_runs, base.m_bytes / (BATCH + 100));
    BOOST_CHECK_LE(base.m_runs, base.m_bytes / BATCH + 9);
    store.SanityCheck();
}

//! Records the range of txids of every run written through it, in the order the database has its keys in.
class RangeBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    Mutex m_mutex;
    std::vector<std::pair<uint256, uint256>> m_ranges GUARDED_BY(m_mutex);
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        if (!writes.empty()) {
            const auto [lo, hi]{std::ranges::minmax_element(writes, {}, [](const CoinWrite& write) { return write.outpoint.hash.ToUint256(); })};
            LOCK(m_mutex);
            m_ranges.emplace_back(lo->outpoint.hash.ToUint256(), hi->outpoint.hash.ToUint256());
        }
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A flush of shards that are small against a batch writes batches of whole neighbouring shards: ranges
//! of the database's keys that do not reach into one another, from several workers.
BOOST_AUTO_TEST_CASE(flush_batches_are_disjoint_key_ranges)
{
    static constexpr uint64_t BATCH{20000};
    CCoinsViewDB db{MemoryDb("store"), {}};
    RangeBase base{&db};
    CoinsStore store{&base, 64};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, Coin> expected;
    for (int i{0}; i < 4000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), uint32_t(i % 3)};
        Coin coin{RandomCoin(m_rng, 1)};
        expected[outpoint] = coin;
        store.AddCoin(outpoint, std::move(coin), false);
    }
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    // A shard's share is a fifth of a batch or so: a batch is a few shards, and there are many batches.
    BOOST_CHECK_GE(base.m_ranges.size(), 8U);
    BOOST_CHECK_LE(base.m_ranges.size(), 64U);
    std::ranges::sort(base.m_ranges);
    for (size_t i{1}; i < base.m_ranges.size(); ++i) {
        BOOST_CHECK_MESSAGE(base.m_ranges[i - 1].second < base.m_ranges[i].first, "batches " << i - 1 << " and " << i << " overlap");
    }
}

//! A run that stops at a bound ends by recording the settled head in the database: the old head on disk is
//! the block the tier counts as written, never the head of the run before.
BOOST_AUTO_TEST_CASE(streamed_run_records_the_settled_head)
{
    CCoinsViewDB db{{.path = "store", .cache_bytes = 1_MiB, .memory_only = true}, {}};
    CoinsStore store{&db, 4, true};
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    const auto add{[&](int height) {
        store.SetBestBlock(hash(height), height);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + height)}), 0}, RandomCoin(m_rng, height), false);
    }};
    for (int height{1}; height <= 4; ++height) add(height);

    auto run{store.BeginWriteOut(/*bound_height=*/2, /*head_height=*/4, hash(4), /*final=*/false)};
    BOOST_REQUIRE(run);
    store.StreamWriteOut(*run, hash);
    store.CommitWriteOut(*run);
    BOOST_CHECK_EQUAL(run->count, 2U);
    BOOST_CHECK_EQUAL(run->settled_height, 2);
    BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(4), hash(2)}));
    BOOST_CHECK(store.WrittenHead() == hash(2));

    // The next run starts from that head and leaves its own.
    add(5);
    run = store.BeginWriteOut(/*bound_height=*/3, /*head_height=*/5, hash(5), /*final=*/false);
    BOOST_REQUIRE(run);
    BOOST_CHECK(run->old_head == hash(2));
    store.StreamWriteOut(*run, hash);
    store.CommitWriteOut(*run);
    BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(5), hash(3)}));
    BOOST_CHECK(store.WrittenHead() == hash(3));
    store.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
