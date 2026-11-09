// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coinsstore.h>
#include <dbwrapper.h>
#include <script/script.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <util/threadpool.h>
#include <util/byte_units.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

//! Parameters of an in-memory LevelDB for a test, every field named.
static DBParams MemoryDb(const char* name)
{
    return DBParams{
        .path = name,
        .cache_bytes = 1_MiB,
        .memory_only = true,
        .wipe_data = false,
        .obfuscate = false,
        .bloom_filter = true,
        .options = {},
    };
}

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
    CCoinsViewDB db_cache{MemoryDb("cache"), {}};
    CCoinsViewDB db_store{MemoryDb("store"), {}};
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
    CCoinsViewDB db{MemoryDb("store"), {}};
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
    CCoinsViewDB db{MemoryDb("store"), {}};
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
    CCoinsViewDB db{MemoryDb("store"), {}};
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
    CCoinsViewDB db{MemoryDb("store"), {}};
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
    CCoinsViewDB db{MemoryDb("store"), {}};
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
    CCoinsViewDB db{MemoryDb("store"), {}};
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
    CCoinsViewDB db{MemoryDb("store"), {}};
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

//! Changes made under a block's identity are seen by appliers and not by readers until the block is
//! confirmed; unwinding makes them disappear; conflicts are told apart by height.
BOOST_AUTO_TEST_CASE(block_identity_visibility_and_conflicts)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint old_coin{Txid::FromUint256(uint256{10}), 0};
    const COutPoint new_coin{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(old_coin, RandomCoin(m_rng, 1), false); // a confirmed coin...
    store.Sync();                                         // ...that is in the database, so spending it owes an erase

    const auto b2{store.BeginBlock(2)};
    const auto b3{store.BeginBlock(3)};
    BOOST_CHECK_EQUAL(store.OpenBlocks(), 2U);

    // Block 2 creates new_coin and spends old_coin.
    store.ApplyCreate(b2, new_coin, RandomCoin(m_rng, 2), false);
    BOOST_CHECK(store.ApplySpend(b2, old_coin, nullptr));
    // Readers: the create is invisible, the old coin still there.
    BOOST_CHECK(!store.HaveCoin(new_coin));
    BOOST_CHECK(store.HaveCoin(old_coin));
    // Appliers (block 3): the create is visible, the old coin spent.
    BOOST_CHECK(store.LookupFor(b3, new_coin).has_value());
    BOOST_CHECK(!store.LookupFor(b3, old_coin).has_value());
    // Block 3 spending old_coin too: block 2 is lower, block 3 is the offender.
    BOOST_CHECK(!store.ApplySpend(b3, old_coin, nullptr));
    BOOST_CHECK_EQUAL(store.ConflictCount(b3), 1U);
    BOOST_CHECK_EQUAL(store.ConflictCount(b2), 0U);
    // Nothing of this is writable yet.
    BOOST_CHECK(!store.PrepareWriteOut(10, 3, uint256{3}, false));
    store.SanityCheck();

    // Confirm block 2: readers now see the create and the spend; the entries are dirty and the run's to take.
    store.ConfirmBlock(b2);
    BOOST_CHECK(store.HaveCoin(new_coin));
    BOOST_CHECK(!store.HaveCoin(old_coin));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 2U); // new_coin to write, old_coin's erase
    auto run{store.PrepareWriteOut(10, 2, uint256{2}, false)};
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 2U);
    run->settled_head = uint256{2};
    store.WriteRun(run->writes, run->new_head, run->old_head, false);
    store.CommitWriteOut(*run);
    BOOST_CHECK(db.PeekCoin(new_coin).has_value());
    BOOST_CHECK(!db.PeekCoin(old_coin).has_value());

    // Block 3 (the offender) is unwound: nothing of it remains. A fresh block 4 spends new_coin and is
    // then unwound: the coin is back for everyone.
    store.UnwindBlock(b3);
    const auto b4{store.BeginBlock(4)};
    BOOST_CHECK(store.ApplySpend(b4, new_coin, nullptr));
    BOOST_CHECK(store.HaveCoin(new_coin));             // readers still see it
    BOOST_CHECK(!store.LookupFor(b4, new_coin));       // the applier does not
    store.UnwindBlock(b4);
    BOOST_CHECK(store.HaveCoin(new_coin));
    BOOST_CHECK_EQUAL(store.OpenBlocks(), 0U);
    store.SanityCheck();

    // Displacement: a higher block spends first, a lower block then takes the coin; the higher block gets
    // the conflict.
    const auto b6{store.BeginBlock(6)};
    const auto b5{store.BeginBlock(5)};
    BOOST_CHECK(store.ApplySpend(b6, new_coin, nullptr));
    BOOST_CHECK(store.ApplySpend(b5, new_coin, nullptr));
    BOOST_CHECK_EQUAL(store.ConflictCount(b6), 1U);
    BOOST_CHECK_EQUAL(store.ConflictCount(b5), 0U);
    store.ConfirmBlock(b5);
    BOOST_CHECK(!store.HaveCoin(new_coin));
    store.UnwindBlock(b6);
    store.SanityCheck();
    // A coin created and spent inside the window leaves no trace once confirmed.
    const auto b7{store.BeginBlock(7)};
    const COutPoint ephemeral{Txid::FromUint256(uint256{12}), 0};
    store.ApplyCreate(b7, ephemeral, RandomCoin(m_rng, 7), false);
    BOOST_CHECK(store.ApplySpend(b7, ephemeral, nullptr));
    store.ConfirmBlock(b7);
    BOOST_CHECK(!store.HaveCoinInCache(ephemeral));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U); // only new_coin's erase from block 5
    store.SanityCheck();
}

//! A confirmed coin that an open block spends is written by a run as the confirmed coin it still is, and the
//! settled height is not held down by it; the spend lands as an erase once the block confirms, and nothing
//! is owed if the block is unwound.
BOOST_AUTO_TEST_CASE(run_writes_a_confirmed_coin_an_open_block_spends)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false); // confirmed, dirty, tagged 1
    store.AddCoin(b, RandomCoin(m_rng, 1), false);

    const auto b2{store.BeginBlock(2)};
    BOOST_CHECK(store.ApplySpend(b2, a, nullptr)); // a is held by block 2 now, still unspent for readers
    BOOST_CHECK(store.HaveCoin(a));

    auto run{store.PrepareWriteOut(/*bound_height=*/1, /*head_height=*/1, uint256{1}, /*final=*/false)};
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 2U); // a and b, both as coins
    for (const auto& w : run->writes) BOOST_CHECK(w.coin.has_value());
    BOOST_CHECK_EQUAL(run->settled_height, 1);
    run->settled_head = uint256{1};
    store.WriteRun(run->writes, run->new_head, run->old_head, run->final);
    store.CommitWriteOut(*run);
    BOOST_CHECK(db.HaveCoin(a));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);

    // Block 2 confirms: the spend is a change the database does not have, so an erase is owed and written.
    store.ConfirmBlock(b2);
    store.SetBestBlock(uint256{2}, 2);
    BOOST_CHECK(!store.HaveCoin(a));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(a));
    BOOST_CHECK(db.HaveCoin(b));

    // The block confirming before the run lands: the spend changed the entry (spent now), so the run must
    // not settle it, and the erase stays owed.
    const COutPoint c{Txid::FromUint256(uint256{12}), 0};
    store.AddCoin(c, RandomCoin(m_rng, 2), false);
    const auto b3c{store.BeginBlock(3)};
    BOOST_CHECK(store.ApplySpend(b3c, c, nullptr));
    auto run_c{store.PrepareWriteOut(2, 2, uint256{2}, false)};
    BOOST_REQUIRE(run_c);
    BOOST_CHECK_EQUAL(run_c->writes.size(), 1U);
    store.ConfirmBlock(b3c);
    store.SetBestBlock(uint256{3}, 3);
    run_c->settled_head = uint256{2};
    store.WriteRun(run_c->writes, run_c->new_head, run_c->old_head, run_c->final);
    store.CommitWriteOut(*run_c);
    BOOST_CHECK(db.HaveCoin(c));                   // the run wrote the coin
    BOOST_CHECK(!store.HaveCoin(c));               // spent in the tier
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);  // the erase is still owed
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(c));

    // The same with the block unwound instead: the coin stays, clean, as the database has it.
    const auto b3{store.BeginBlock(3)};
    BOOST_CHECK(store.ApplySpend(b3, b, nullptr));
    auto run2{store.PrepareWriteOut(2, 2, uint256{2}, false)};
    BOOST_CHECK(!run2); // b was written already and is clean: nothing to carry
    store.UnwindBlock(b3);
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    store.SanityCheck();
}

//! A block that creates an outpoint over an erase the database is still owed keeps that erase on the books:
//! a run meanwhile writes it, the outpoint's confirmed value, so that the database a final run leaves is the
//! head's state; unwritten, it is owed still if the block is unwound and written over if the block confirms.
BOOST_AUTO_TEST_CASE(owed_erase_survives_a_create_and_its_unwind)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync(); // x is in the database
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(x)); // spent, dirty: the erase is owed
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 2);

    // An applied block re-creates x (a duplicate, as BIP30 allowed once), then is unwound.
    const auto b3{store.BeginBlock(3)};
    store.ApplyCreate(b3, x, RandomCoin(m_rng, 3), false);
    BOOST_CHECK(!store.HaveCoin(x));          // readers: still spent
    BOOST_CHECK(store.LookupFor(b3, x).has_value());
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 2); // the owed erase keeps its tag
    store.UnwindBlock(b3);
    BOOST_CHECK(!store.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);

    // Applied again, with a final run while the block is undecided: the erase lands, the block's coin stays
    // the block's, and nothing is owed when the block is unwound.
    const auto b3b{store.BeginBlock(3)};
    store.ApplyCreate(b3b, x, RandomCoin(m_rng, 3), false);
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    BOOST_CHECK(!store.HaveCoin(x));
    BOOST_CHECK(store.LookupFor(b3b, x).has_value());
    store.SanityCheck();
    store.UnwindBlock(b3b);
    BOOST_CHECK(!store.HaveCoinInCache(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);

    // The same create confirming instead writes the new coin over the old.
    store.SetChangeHeight(4);
    store.AddCoin(x, RandomCoin(m_rng, 4), false);
    store.Sync();
    store.SetChangeHeight(5);
    BOOST_CHECK(store.SpendCoin(x));
    const auto b6{store.BeginBlock(6)};
    store.ApplyCreate(b6, x, RandomCoin(m_rng, 6), false);
    store.ConfirmBlock(b6);
    store.SetBestBlock(uint256{6}, 6);
    BOOST_CHECK(store.HaveCoin(x));
    BOOST_CHECK_EQUAL(*store.OldestDirtyHeight(), 6);
    store.Sync();
    const auto coin{db.PeekCoin(x)};
    BOOST_REQUIRE(coin.has_value());
    BOOST_CHECK_EQUAL(coin->nHeight, 6U);
    store.SanityCheck();
}

//! Confirming with a pool flips the status at once and frees the block's record on the pool: readers see
//! the block immediately, a run finds its changes dirty where the block left them, and the identity is
//! reusable afterwards.
BOOST_AUTO_TEST_CASE(confirm_frees_the_record_on_the_pool)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    ThreadPool pool{"record"};
    pool.Start(2);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    store.Sync();

    for (int round{0}; round < 50; ++round) {
        const auto id{store.BeginBlock(2 + round)};
        const COutPoint made{Txid::FromUint256(uint256{static_cast<uint8_t>(20 + round)}), 0};
        store.ApplyCreate(id, made, RandomCoin(m_rng, 2 + round), false);
        if (round == 0) BOOST_CHECK(store.ApplySpend(id, a, nullptr));
        store.ConfirmBlock(id, &pool);
        BOOST_CHECK(store.HaveCoin(made)); // visible from the flip
    }
    BOOST_CHECK(!store.HaveCoin(a));
    // A run needs no wait: everything was dirty from the moment it was applied.
    auto run{store.PrepareWriteOut(/*bound_height=*/60, /*head_height=*/51, uint256{51}, /*final=*/false)};
    BOOST_REQUIRE(run);
    BOOST_CHECK_EQUAL(run->writes.size(), 51U); // 50 creates and a's erase
    BOOST_CHECK_EQUAL(store.OpenBlocks(), 0U);
    run->settled_head = uint256{51};
    store.WriteRun(run->writes, run->new_head, run->old_head, run->final);
    store.CommitWriteOut(*run);
    BOOST_CHECK(!db.HaveCoin(a));
    BOOST_CHECK(db.HaveCoin(b) == false);
    store.SanityCheck();
    pool.Stop();
}

//! A higher block that creates an outpoint before the lower block that creates it too (a duplicate the
//! higher block could not see, BIP30) is the one charged: the lower block takes the entry, the higher
//! block fails at its confirmation.
BOOST_AUTO_TEST_CASE(higher_block_creating_first_is_charged)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint dup{Txid::FromUint256(uint256{10}), 0};

    const auto b2{store.BeginBlock(2)};
    const auto b3{store.BeginBlock(3)};
    store.ApplyCreate(b3, dup, RandomCoin(m_rng, 3), false); // block 3 first: nothing of block 2 is visible to it
    BOOST_CHECK_EQUAL(store.ConflictCount(b3), 0U);
    store.ApplyCreate(b2, dup, RandomCoin(m_rng, 2), false); // block 2 creates the same outpoint
    BOOST_CHECK_EQUAL(store.ConflictCount(b3), 1U);
    BOOST_CHECK_EQUAL(store.ConflictCount(b2), 0U);
    BOOST_CHECK(store.LookupFor(b2, dup).has_value());

    // Block 2 confirms; block 3 is unwound as a failed block, and the coin is block 2's.
    store.ConfirmBlock(b2);
    store.UnwindBlock(b3);
    const auto coin{store.PeekCoin(dup)};
    BOOST_REQUIRE(coin.has_value());
    BOOST_CHECK_EQUAL(coin->nHeight, 2U);
    store.SanityCheck();
}

//! The oracle: synthetic blocks applied in a random order inside a window, with spends of coins not yet
//! visible deferred to confirmation, confirm in height order to the same set an in-order cache reaches.
BOOST_AUTO_TEST_CASE(random_order_window_matches_in_order)
{
    for (int trial{0}; trial < 20; ++trial) {
        constexpr uint32_t TXIDS{40};
        const auto universe{Universe(TXIDS)};
        CCoinsViewDB db_cache{MemoryDb("cache"), {}};
        CCoinsViewDB db_store{MemoryDb("store"), {}};
        CCoinsViewCache cache{&db_cache};
        CoinsStore store{&db_store, 8, true};
        cache.SetBestBlock(uint256::ONE);
        store.SetBestBlock(uint256::ONE, 0);
        // Seed some confirmed coins in both.
        for (uint32_t i{0}; i < 20; ++i) {
            const COutPoint op{universe[i]};
            Coin a{RandomCoin(m_rng, 0)};
            Coin b{a};
            cache.AddCoin(op, std::move(a), false);
            store.AddCoin(op, std::move(b), false);
        }

        // A chain of synthetic blocks: each spends a few coins visible at its height (in order) and creates
        // a few new ones. Built in order against a scratch cache so every block is valid in order.
        struct SynthBlock {
            int height;
            std::vector<COutPoint> spends;
            std::vector<std::pair<COutPoint, Coin>> creates;
        };
        std::vector<SynthBlock> chain;
        {
            std::vector<COutPoint> live(universe.begin(), universe.begin() + 20);
            size_t next_fresh{20}; // outpoints are never reused: a repeat would be a BIP30 duplicate
            for (int h{1}; h <= 12; ++h) {
                SynthBlock b{.height = h, .spends = {}, .creates = {}};
                for (int k{0}; k < 3 && !live.empty(); ++k) {
                    const size_t idx{m_rng.randrange(live.size())};
                    b.spends.push_back(live[idx]);
                    live.erase(live.begin() + idx);
                }
                for (int k{0}; k < 3 && next_fresh < universe.size(); ++k) {
                    const COutPoint op{universe[next_fresh++]};
                    b.creates.emplace_back(op, RandomCoin(m_rng, h));
                    live.push_back(op);
                }
                chain.push_back(std::move(b));
            }
        }

        // In order, into the cache: the reference.
        for (const SynthBlock& b : chain) {
            for (const auto& [op, coin] : b.creates) cache.AddCoin(op, Coin{coin}, false);
            for (const COutPoint& op : b.spends) BOOST_CHECK(cache.SpendCoin(op));
        }

        // Out of order, into the store, inside a window of 4 blocks: apply what is visible, defer the rest,
        // confirm in height order running the deferred spends then.
        std::vector<CoinsStore::BlockId> ids(chain.size(), CoinsStore::NO_BLOCK);
        std::vector<std::vector<COutPoint>> deferred(chain.size());
        size_t next_to_confirm{0};
        while (next_to_confirm < chain.size()) {
            std::vector<size_t> candidates;
            for (size_t i{next_to_confirm}; i < std::min(chain.size(), next_to_confirm + 4); ++i) {
                if (ids[i] == CoinsStore::NO_BLOCK) candidates.push_back(i);
            }
            const bool lowest_applied{ids[next_to_confirm] != CoinsStore::NO_BLOCK};
            if (!candidates.empty() && (!lowest_applied || m_rng.randbool())) {
                const size_t i{candidates[m_rng.randrange(candidates.size())]};
                const SynthBlock& b{chain[i]};
                ids[i] = store.BeginBlock(b.height);
                for (const auto& [op, coin] : b.creates) store.ApplyCreate(ids[i], op, Coin{coin}, false);
                for (const COutPoint& op : b.spends) {
                    if (store.LookupFor(ids[i], op)) {
                        BOOST_CHECK(store.ApplySpend(ids[i], op, nullptr));
                    } else {
                        deferred[i].push_back(op); // created by a lower block not applied yet
                    }
                }
                continue;
            }
            // Confirm the lowest block: every lower block is confirmed, so the deferred spends find their coins.
            const size_t i{next_to_confirm};
            for (const COutPoint& op : deferred[i]) {
                BOOST_REQUIRE(store.LookupFor(ids[i], op).has_value());
                BOOST_CHECK(store.ApplySpend(ids[i], op, nullptr));
            }
            BOOST_CHECK_EQUAL(store.ConflictCount(ids[i]), 0U);
            store.ConfirmBlock(ids[i]);
            ++next_to_confirm;
        }
        CheckSameVisible(cache, store, universe);
        store.SanityCheck();
        cache.Flush();
        store.Flush();
        CheckSameVisible(db_cache, db_store, universe);
    }
}

//! The block view: changes made through it land in the store under the block's identity, in-block
//! dependencies resolve through the view's own map, and confirm/unwind are the store's status flips.
//! The batch: a block's creates and spends applied in one pass per shard behave as the one-by-one calls do,
//! including the conflict with a lower block's spend and a spend whose entry left the tier meanwhile.
BOOST_AUTO_TEST_CASE(apply_batch_matches_one_by_one)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    const COutPoint c{Txid::FromUint256(uint256{12}), 0};
    const COutPoint d{Txid::FromUint256(uint256{20}), 0};
    const COutPoint unknown{Txid::FromUint256(uint256{99}), 0};
    for (const auto& outpoint : {a, b, c}) store.AddCoin(outpoint, RandomCoin(m_rng, 1), false);
    store.Sync(); // the database has them too

    // FetchFor: what the block sees among what the tier holds; b no longer, once a lower block spent it.
    const auto lower{store.BeginBlock(2)};
    const auto higher{store.BeginBlock(3)};
    BOOST_CHECK_EQUAL(store.FetchFor(higher, {a, b, c, unknown}).size(), 3U);
    BOOST_CHECK(store.ApplySpend(lower, b, nullptr));
    BOOST_CHECK_EQUAL(store.FetchFor(higher, {a, b, c}).size(), 2U);

    // The higher block spends a and b (the lower block's: a conflict) and creates d, in one batch.
    std::vector<CoinsStore::Change> changes;
    changes.push_back({.outpoint = a, .coin = Coin{}, .possible_overwrite = false}); // a spent coin: a spend
    changes.push_back({.outpoint = b, .coin = Coin{}, .possible_overwrite = false});
    changes.push_back({.outpoint = d, .coin = RandomCoin(m_rng, 3), .possible_overwrite = false});
    store.ApplyBatch(higher, std::move(changes));
    BOOST_CHECK_EQUAL(store.ConflictCount(higher), 1U);
    BOOST_CHECK_EQUAL(store.ConflictCount(lower), 0U);
    BOOST_CHECK(!store.PeekCoinFor(higher, a)); // spent, as the block sees it
    BOOST_CHECK(store.HaveCoin(a));            // readers see nothing until confirmation
    BOOST_CHECK(store.PeekCoinFor(higher, d));
    BOOST_CHECK(!store.HaveCoin(d));

    // The lower block confirms; the higher one, conflicted, is unwound: a is back, d never was.
    store.ConfirmBlock(lower);
    store.UnwindBlock(higher);
    BOOST_CHECK(store.HaveCoin(a));
    BOOST_CHECK(!store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(d));
    store.SanityCheck();

    // A spend whose entry is not in the tier any more (evicted after the block read it): the batch reads
    // the base for it, one by one.
    const auto late{store.BeginBlock(3)};
    store.EvictClean(0);
    BOOST_CHECK(!store.HaveCoinInCache(a));
    std::vector<CoinsStore::Change> late_changes;
    late_changes.push_back({.outpoint = a, .coin = Coin{}, .possible_overwrite = false});
    store.ApplyBatch(late, std::move(late_changes));
    BOOST_CHECK_EQUAL(store.ConflictCount(late), 0U);
    BOOST_CHECK(!store.PeekCoinFor(late, a));
    store.ConfirmBlock(late);
    BOOST_CHECK(!store.HaveCoin(a));
    store.SanityCheck();
}

//! The view applies the block's changes to the store at confirmation (its worker does so earlier in the
//! window); readers see nothing before, everything after, and an unwound block leaves no trace.
BOOST_AUTO_TEST_CASE(block_apply_view_applies_at_confirm)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint old_coin{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(old_coin, RandomCoin(m_rng, 1), false);
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    // A block that spends old_coin, creates a and b, and spends a within itself.
    const COutPoint a{Txid::FromUint256(uint256{20}), 0};
    const COutPoint b{Txid::FromUint256(uint256{21}), 0};
    view.BeginBlock(2);
    BOOST_CHECK(view.HaveCoin(old_coin));
    Coin spent;
    BOOST_CHECK(view.SpendCoin(old_coin, &spent));
    BOOST_CHECK(!spent.IsSpent());
    view.AddCoin(a, RandomCoin(m_rng, 2), false);
    view.AddCoin(b, RandomCoin(m_rng, 2), false);
    BOOST_CHECK(view.HaveCoin(a));
    BOOST_CHECK(view.SpendCoin(a));
    BOOST_CHECK(!view.HaveCoin(a));
    view.SetBestBlock(uint256{2});
    // Readers see nothing of it yet.
    BOOST_CHECK(store.HaveCoin(old_coin));
    BOOST_CHECK(!store.HaveCoin(b));
    BOOST_CHECK_EQUAL(view.Conflicts(), 0U);
    view.Confirm();
    BOOST_CHECK(!view.HasBlock());
    BOOST_CHECK(!store.HaveCoin(old_coin));
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(a)); // created and spent inside the block: no trace
    BOOST_CHECK(store.GetBestBlock() == uint256{2});
    BOOST_CHECK_EQUAL(store.GetChangeHeight(), 2);
    store.SanityCheck();

    // An unwound block leaves the store as it was.
    view.BeginBlock(3);
    const COutPoint c{Txid::FromUint256(uint256{22}), 0};
    view.AddCoin(c, RandomCoin(m_rng, 3), false);
    BOOST_CHECK(view.SpendCoin(b));
    view.Unwind();
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(c));
    BOOST_CHECK_EQUAL(store.OpenBlocks(), 0U);
    store.SanityCheck();

    // A view destroyed with an open block unwinds it.
    {
        BlockApplyView other{store, pool, true};
        other.BeginBlock(3);
        other.AddCoin(c, RandomCoin(m_rng, 3), false);
        BOOST_CHECK_EQUAL(store.OpenBlocks(), 1U);
    }
    BOOST_CHECK_EQUAL(store.OpenBlocks(), 0U);
    BOOST_CHECK(!store.HaveCoinInCache(c));
}

//! A base whose first read of one outpoint is overtaken: `overtake` runs after the coin was read and before
//! it is returned, as another thread can between an applier's read of the base and its insert.
class OvertakenBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    COutPoint m_watched;
    mutable std::function<void()> m_overtake;
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        auto coin{CCoinsViewBacked::GetCoin(outpoint)};
        if (outpoint == m_watched && m_overtake) std::exchange(m_overtake, nullptr)();
        return coin;
    }
};

//! A higher block reads a coin from the base; before it inserts it, a lower block spends the coin, is
//! confirmed and has the erase written out and the entry dropped. The coin read is stale: the higher
//! block's spend must read again and find the conflict, not put the coin back.
BOOST_AUTO_TEST_CASE(spend_reads_the_base_again_after_a_settle)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    OvertakenBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint coin{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(coin, RandomCoin(m_rng, 1), false);
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U); // the coin is in the database only

    const auto b2{store.BeginBlock(2)};
    const auto b3{store.BeginBlock(3)};
    base.m_watched = coin;
    base.m_overtake = [&] {
        BOOST_CHECK(store.ApplySpend(b2, coin, nullptr));
        store.ConfirmBlock(b2);
        store.SetBestBlock(uint256{2}, 2);
        store.Sync();
        BOOST_CHECK(!db.PeekCoin(coin).has_value());
    };
    BOOST_CHECK(!store.ApplySpend(b3, coin, nullptr));
    BOOST_CHECK(!base.m_overtake);
    BOOST_CHECK_EQUAL(store.ConflictCount(b3), 1U);
    store.UnwindBlock(b3);
    store.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
