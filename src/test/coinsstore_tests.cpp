// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coinsstore.h>
#include <dbwrapper.h>
#include <primitives/block.h>
#include <script/script.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <util/threadpool.h>
#include <util/byte_units.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <thread>
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

//! A started pool for a flush's further workers, as the node gives the store its connect pool.
static std::shared_ptr<ThreadPool> FlushHelpers()
{
    auto pool{std::make_shared<ThreadPool>("flush")};
    pool->Start(3);
    return pool;
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

//! Block `block` creates `outpoint`: a batch of the one change.
void Create(CoinsStore& store, CoinsStore::BlockId block, const COutPoint& outpoint, Coin coin, bool possible_overwrite)
{
    std::vector<CoinsStore::Change> changes;
    changes.push_back({.outpoint = outpoint, .coin = std::move(coin), .possible_overwrite = possible_overwrite});
    store.ApplyBatch(block, std::move(changes));
}

//! A name for the block at `height`: any but the null hash.
uint256 BlockLabel(int height) { return uint256{static_cast<uint8_t>(1 + height % 255)}; }

//! A write-out run as the node makes one: begun, streamed to the base and committed.
std::unique_ptr<CoinsStore::WriteOutRun> WriteOut(CoinsStore& store, int bound_height, int head_height, const uint256& new_head, const std::function<uint256(int)>& block_at = BlockLabel)
{
    auto run{store.BeginWriteOut(bound_height, head_height, new_head, /*final=*/false)};
    BOOST_REQUIRE(run);
    store.StreamWriteOut(*run, block_at);
    store.CommitWriteOut(*run);
    return run;
}

//! A base that keeps the changes of the latest run written through it, and calls `m_in_flight` once while a
//! run is in flight: after the run collected its changes and before the first of them is in the database
//! (the tests' runs are a single batch).
class RunBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    std::vector<CoinWrite> m_writes;
    std::function<void()> m_in_flight;
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (m_in_flight) std::exchange(m_in_flight, nullptr)();
        // A run ends with a write of no changes, which closes or moves its marker.
        if (std::exchange(m_closed, writes.empty())) m_writes.clear();
        m_writes.insert(m_writes.end(), writes.begin(), writes.end());
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }

private:
    bool m_closed{false};
};

//! The lowest change height among the dirty entries, or nullopt if there is none.
std::optional<int> OldestDirty(const CoinsStore& store)
{
    const int bound{store.BoundForBytes(1, std::numeric_limits<int>::max())};
    return bound == std::numeric_limits<int>::max() ? std::nullopt : std::make_optional(bound);
}

//! Bytes of the dirty entries changed at or below `height`: the most that a bound no higher frees.
size_t DirtyBytesAtOrBelow(const CoinsStore& store, int height)
{
    const auto within{[&](size_t bytes) { return store.BoundForBytes(bytes, std::numeric_limits<int>::max()) <= height; }};
    size_t low{0};
    size_t high{1};
    while (within(high)) high *= 2;
    while (high - low > 1) {
        const size_t mid{low + (high - low) / 2};
        (within(mid) ? low : high) = mid;
    }
    return low;
}

} // namespace

//! The store, driven as a cache would be (adds, spends, child flushes, full flushes), shows the same coins
//! as a CCoinsViewCache over an identical database at every step. After a bounded run its database holds
//! every outpoint as it is, unless the outpoint changed above the height the run settled at.
BOOST_AUTO_TEST_CASE(same_as_cache_under_random_operations)
{
    constexpr uint32_t TXIDS{24};
    const auto universe{Universe(TXIDS)};
    CCoinsViewDB db_cache{MemoryDb("cache"), {}};
    CCoinsViewDB db_store{MemoryDb("store"), {}};
    CCoinsViewCache cache{&db_cache};
    RunBase base{&db_store};
    CoinsStore store{&base, /*shards=*/8, /*deterministic=*/true};
    const uint256 genesis{m_rng.rand256()};
    cache.SetBestBlock(genesis);
    store.SetBestBlock(genesis, 0);
    // The step each outpoint was last added or spent at.
    std::map<COutPoint, int> last_change;

    for (int step{1}; step < 400; ++step) {
        const uint256 block_hash{m_rng.rand256()};
        store.SetChangeHeight(step);
        // Some operations straight on the tier, some through a child cache as a block connection does.
        if (m_rng.randbool()) {
            for (int i{0}; i < 6; ++i) {
                const COutPoint op{RandomOutpoint(m_rng, TXIDS)};
                last_change[op] = step;
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
                last_change[op] = step;
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
                auto run{store.BeginWriteOut(bound, step, block_hash, /*final=*/false)};
                if (run) {
                    // Changes made while the run is in flight stay dirty.
                    const COutPoint late{RandomOutpoint(m_rng, TXIDS)};
                    bool late_spent{false};
                    base.m_in_flight = [&] {
                        late_spent = store.HaveCoin(late);
                        store.SpendCoin(late);
                        cache.SpendCoin(late);
                    };
                    store.StreamWriteOut(*run, [&](int height) { return height == step ? block_hash : BlockLabel(height); });
                    store.CommitWriteOut(*run);
                    BOOST_CHECK(run->settled_height >= bound && run->settled_height <= step);
                    BOOST_CHECK(store.WrittenHeight() == run->settled_height);
                    BOOST_CHECK(store.WrittenHead() == run->settled_head);
                    const auto heads{db_store.GetHeadBlocks()};
                    BOOST_REQUIRE_EQUAL(heads.size(), 2U);
                    BOOST_CHECK(heads[0] == block_hash);
                    BOOST_CHECK(db_store.GetBestBlock().IsNull());
                    // Nothing changed at or below the bound is still dirty, unless it changed again since.
                    const auto oldest{OldestDirty(store)};
                    BOOST_CHECK(!oldest || *oldest > bound || late_spent);
                    // The database holds the state at the settled height: every outpoint not changed above
                    // it is there as the views have it. (An outpoint changed above it is the replay's, which
                    // applies that change again; the outpoint spent with the run in flight is one such.)
                    for (const COutPoint& op : universe) {
                        if (op == late) continue;
                        if (const auto changed{last_change.find(op)}; changed != last_change.end() && changed->second > run->settled_height) continue;
                        BOOST_CHECK_MESSAGE(Same(db_store.PeekCoin(op), store.PeekCoin(op)),
                                            "after a run settled at " << run->settled_height << " the database differs at " << op.ToString());
                    }
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
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
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
    BOOST_CHECK_EQUAL(*OldestDirty(store), 301);

    // A run asked to stop at 301 is final here: the tier fell below the written head (SetTierHeight()), and
    // the changes that take the database back are tagged with the fork. It carries both and leaves nothing
    // dirty: the database then holds 306's state, and that, not 301, is what the next marker must name as
    // its old head.
    auto run{WriteOut(store, /*bound_height=*/301, /*head_height=*/306, hash(306), hash)};
    BOOST_CHECK(run->final);
    BOOST_CHECK_EQUAL(run->count, 2U);
    BOOST_CHECK(run->old_head == hash(307));
    BOOST_CHECK_EQUAL(run->settled_height, 306);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 306);
    BOOST_CHECK(store.WrittenHead() == hash(306));
    BOOST_CHECK(!OldestDirty(store));

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
    auto run2{WriteOut(store, /*bound_height=*/306, /*head_height=*/307, hash(57), hash)};
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(base.m_writes[0].outpoint == outpoint(304));
    BOOST_CHECK(run2->old_head == hash(306));
    // 307' remains dirty (its coinbase and the restored coin), so the database settles at 306 still.
    BOOST_CHECK_EQUAL(run2->settled_height, 306);
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

//! A coin created and spent above the written head cancels in the tier. Disconnecting the spend restores
//! it while the change height is the fork height, which is at or above its creation: it is tagged with
//! its creation height, so that a run bounded between the two carries it. Tagged with the fork height it
//! would be left behind while the run's settled head passed its creation, and a crash in the next run
//! would replay from above the block that creates it, which nothing then writes again.
BOOST_AUTO_TEST_CASE(restored_coin_keeps_its_creation_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    const auto hash{[](int h) { return uint256{static_cast<uint8_t>(h - 200)}; }};
    const auto coin{[](int h) { return Coin{CTxOut{CAmount{h}, CScript() << OP_TRUE}, h, false}; }};
    const COutPoint c{Txid::FromUint256(uint256{77}), 0};

    store.SetBestBlock(hash(300), 300);
    store.Sync(); // the database holds the state at 300
    BOOST_REQUIRE_EQUAL(store.WrittenHeight(), 300);
    // 301 to 305; c is created in 302 and spent in 304, each block also creates a coin of its own.
    for (int h{301}; h <= 305; ++h) {
        store.SetBestBlock(hash(h), h);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(h - 200)}), 0}, coin(h), false);
        if (h == 302) store.AddCoin(c, coin(302), false);
        if (h == 304) BOOST_REQUIRE(store.SpendCoin(c));
    }
    BOOST_CHECK(!store.HaveCoinInCache(c));

    // Disconnect 305 and 304 towards a fork at 303: 304's spend of c is undone.
    store.SetChangeHeight(303);
    BOOST_REQUIRE(store.SpendCoin(COutPoint{Txid::FromUint256(uint256{105}), 0}));
    BOOST_REQUIRE(store.SpendCoin(COutPoint{Txid::FromUint256(uint256{104}), 0}));
    store.AddCoin(c, coin(302), false);
    store.SetBestBlock(hash(303));
    store.SetTierHeight(303);

    // A run bounded at 302 settles the database at 302 or below unless it carries c.
    WriteOut(store, /*bound_height=*/302, /*head_height=*/303, hash(303), hash);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 302);
    BOOST_CHECK(db.HaveCoin(c));
    store.SanityCheck();
}

//! A coin created and spent inside the lag is never written, not even as an erase, and a spent entry
//! that was written is erased on the next run.
BOOST_AUTO_TEST_CASE(fresh_coins_leave_no_trace)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    store.AddCoin(b, RandomCoin(m_rng, 1), false);
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(a)); // fresh: dropped outright
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 1U);
    WriteOut(store, 2, 2, uint256{2});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U); // only b
    BOOST_CHECK(base.m_writes[0].coin.has_value());
    BOOST_CHECK(db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    // b is in the database now; spending it must produce an erase even though it was fresh once.
    store.SetChangeHeight(3);
    BOOST_CHECK(store.SpendCoin(b));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    WriteOut(store, 3, 3, uint256{3});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(!base.m_writes[0].coin.has_value());
    BOOST_CHECK(!db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
}

//! A spend that lands between collection and commit keeps its entry dirty; a run only settles what it saw.
BOOST_AUTO_TEST_CASE(run_settles_only_unchanged_entries)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    base.m_in_flight = [&] {
        store.SetChangeHeight(2);
        BOOST_CHECK(store.SpendCoin(a)); // no longer fresh: the run will put it in the database
    };
    WriteOut(store, 1, 1, uint256{1});
    BOOST_CHECK(db.PeekCoin(a).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U); // the erase is still owed
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);
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
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);
    const size_t per_height{DirtyBytesAtOrBelow(store, 2)};
    BOOST_CHECK(per_height > 0);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height, 11), 2);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 3, 11), 4);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 100, 11), 11);
    auto run{WriteOut(store, 4, 11, uint256{11})};
    BOOST_CHECK_EQUAL(run->count, 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 4); // height 5 stays dirty
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 7U);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 5);
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
    BOOST_CHECK_EQUAL(*OldestDirty(store), 1000);
    const size_t each{DirtyBytesAtOrBelow(store, 1000)};
    BOOST_CHECK(each > 0);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 999), 0U);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 1029), each * 2);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 1030), each * 3);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 4999), each * 5);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 2, 5000), 1020);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 3, 5000), 1030);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 5, 5000), 2100);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 7, 5000), 5000);

    // A bound inside a span takes the heights at or below it and settles just below the next height, which
    // shares its list with what was written.
    auto run{WriteOut(store, /*bound_height=*/1030, /*head_height=*/5000, uint256{50})};
    BOOST_CHECK_EQUAL(run->count, 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 1039);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 1040);

    // A bound between two spans leaves the lists further up alone.
    run = WriteOut(store, /*bound_height=*/2000, /*head_height=*/5000, uint256{50});
    BOOST_CHECK_EQUAL(run->count, 1U);
    BOOST_CHECK_EQUAL(run->settled_height, 2099);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 2U);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2100);
    store.SanityCheck();
}

//! A base whose write fails once it is told to.
class FailingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    bool m_fail{false};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (m_fail) throw std::runtime_error{"the write failed"};
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A run whose write fails is abandoned and is no longer in flight. What it collected counts as written in
//! the tier and is not in the database, so the tier refuses every write after it.
BOOST_AUTO_TEST_CASE(failed_run_ends_all_writes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    FailingBase base{&db};
    CoinsStore store{&base, 256, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
    store.AddCoin(outpoint, RandomCoin(m_rng, 1), false);
    BOOST_CHECK(!store.FlushFailed());
    base.m_fail = true;
    BOOST_CHECK_THROW(store.Sync(), std::runtime_error);
    BOOST_CHECK(!store.RunInFlight());
    BOOST_CHECK(store.FlushFailed());
    base.m_fail = false;
    BOOST_CHECK_THROW(store.Flush(), std::runtime_error);
    BOOST_CHECK(!db.PeekCoin(outpoint));
    // A forced sync refuses as well: it would close the marker over what the failed run had collected.
    BOOST_CHECK_THROW(store.Sync(), std::runtime_error);
    BOOST_CHECK(!db.PeekCoin(outpoint));
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
    CoinsStore store{&base, 256, true};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
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
    // Batches of whole shards, each within a shard's changes of the batch size, then what each worker
    // had left, and the run that closes the marker: far fewer and far larger than a run for each of the
    // 256 shards would be.
    BOOST_CHECK_GE(base.m_runs, base.m_bytes / (BATCH + 100));
    BOOST_CHECK_LE(base.m_runs, base.m_bytes / (BATCH - 1000) + 5);
    store.SanityCheck();
}

//! Records the range of txids of every run written through it, in the order the database has its keys in.
class RangeBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    Mutex m_mutex;
    std::vector<std::pair<uint256, uint256>> m_ranges GUARDED_BY(m_mutex);
    //! Runs whose changes were not in the descending order of the keys.
    size_t m_unordered GUARDED_BY(m_mutex){0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        if (!writes.empty()) {
            const auto [lo, hi]{std::ranges::minmax_element(writes, {}, [](const CoinWrite& write) { return write.outpoint.hash.ToUint256(); })};
            const bool ordered{std::ranges::is_sorted(writes, [](const CoinWrite& a, const CoinWrite& b) { return b.outpoint < a.outpoint; })};
            LOCK(m_mutex);
            m_ranges.emplace_back(lo->outpoint.hash.ToUint256(), hi->outpoint.hash.ToUint256());
            if (!ordered) ++m_unordered;
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
    store.SetFlushHelpers(FlushHelpers());
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

//! A shard larger than a batch is written as one run in the order of its keys, the outputs of one
//! transaction among themselves included, and never as two runs that both cover its range: the database
//! cuts the run at its batch size, and holds every change.
BOOST_AUTO_TEST_CASE(flush_never_ends_a_batch_inside_a_shard)
{
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{MemoryDb("store"), {.batch_write_bytes = BATCH, .simulate_crash_ratio = 0}};
    RangeBase base{&db};
    CoinsStore store{&base, 4};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, Coin> expected;
    for (int i{0}; i < 500; ++i) {
        const Txid txid{Txid::FromUint256(m_rng.rand256())};
        for (uint32_t n{0}; n < 4; ++n) {
            Coin coin{RandomCoin(m_rng, 1)};
            expected[COutPoint{txid, n}] = coin;
            store.AddCoin(COutPoint{txid, n}, std::move(coin), false);
        }
    }
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    // A shard's share is six batches or so, and it is one run.
    BOOST_CHECK_EQUAL(base.m_ranges.size(), 4U);
    BOOST_CHECK_EQUAL(base.m_unordered, 0U);
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

//! A streamed run whose chunk holds one change, so that every change is a chunk of its own and a chunk ends
//! inside each shard that holds entries, leaves the database, the heads and the tier as a run of one chunk leaves
//! them, once it is committed. The database at a chunk boundary is not looked at.
BOOST_AUTO_TEST_CASE(streamed_run_in_chunks_of_one_change)
{
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    static constexpr int HEIGHTS{6};
    static constexpr int PER_HEIGHT{8};
    const auto outpoint{[](int height, int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(height * 16 + n)}), 0}; }};
    std::vector<Coin> coins;
    for (int height{1}; height <= HEIGHTS; ++height) {
        for (int n{0}; n < PER_HEIGHT; ++n) coins.push_back(RandomCoin(m_rng, height));
    }
    const auto coin{[&](int height, int n) { return coins[(height - 1) * PER_HEIGHT + n]; }};
    const auto add{[&](CoinsStore& store, int height) {
        store.SetBestBlock(hash(height), height);
        for (int n{0}; n < PER_HEIGHT; ++n) store.AddCoin(outpoint(height, n), coin(height, n), false);
    }};

    CCoinsViewDB db_one{MemoryDb("one"), {}};
    CCoinsViewDB db_many{MemoryDb("many"), {}};
    CoinsStore store_one{&db_one, 4, true};
    CoinsStore store_many{&db_many, 4, true};
    // Returns the number of changes the run wrote and the number of chunks it wrote them in.
    const auto run_to{[&](CoinsStore& store, uint64_t batch_bytes) {
        // Two heights that are in the database before the run, so that spends of their coins owe an erase.
        add(store, 1);
        add(store, 2);
        store.Sync();
        add(store, 3);
        for (int n{0}; n < PER_HEIGHT / 2; ++n) BOOST_REQUIRE(store.SpendCoin(outpoint(1, n)));
        add(store, 4);
        add(store, 5);
        // A coin the database holds and one it never held, both spent above the run's bound.
        BOOST_REQUIRE(store.SpendCoin(outpoint(2, 0)));
        BOOST_REQUIRE(store.SpendCoin(outpoint(4, 0)));
        add(store, 6);
        auto run{store.BeginWriteOut(/*bound_height=*/4, /*head_height=*/6, hash(6), /*final=*/false)};
        BOOST_REQUIRE(run);
        store.StreamWriteOut(*run, hash, batch_bytes);
        store.CommitWriteOut(*run);
        store.SanityCheck();
        return std::pair{run->count, run->chunks};
    }};
    const auto [count_one, chunks_one]{run_to(store_one, DEFAULT_DB_CACHE_BATCH)};
    const auto [count_many, chunks_many]{run_to(store_many, /*batch_bytes=*/1)};
    BOOST_CHECK_GT(count_one, 1U);
    BOOST_CHECK_EQUAL(count_one, count_many);
    // The limit is kept: one chunk where it is large, several where it is one byte.
    BOOST_CHECK_EQUAL(chunks_one, 1U);
    BOOST_CHECK_GT(chunks_many, 1U);

    const auto check_equal{[&] {
        BOOST_CHECK(db_one.GetHeadBlocks() == db_many.GetHeadBlocks());
        BOOST_CHECK(db_one.GetBestBlock() == db_many.GetBestBlock());
        BOOST_CHECK(store_one.WrittenHead() == store_many.WrittenHead());
        for (int height{1}; height <= HEIGHTS; ++height) {
            for (int n{0}; n < PER_HEIGHT; ++n) {
                const auto one{db_one.GetCoin(outpoint(height, n))};
                const auto many{db_many.GetCoin(outpoint(height, n))};
                BOOST_REQUIRE_EQUAL(one.has_value(), many.has_value());
                if (one) BOOST_CHECK(one->out == many->out && one->nHeight == many->nHeight && one->fCoinBase == many->fCoinBase);
                BOOST_CHECK_EQUAL(store_one.HaveCoin(outpoint(height, n)), store_many.HaveCoin(outpoint(height, n)));
            }
        }
    }};
    check_equal();
    // The run wrote something of its heights and nothing above its settled head.
    BOOST_CHECK(db_many.GetCoin(outpoint(3, 1)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(6, 1)).has_value());
    // What the bound decides: the coins of the first height spent below it are erased and the others of
    // that height are there, and a coin the database holds that is spent above the bound is there still.
    // A coin made and spent before the database held it is never written.
    for (int n{0}; n < PER_HEIGHT; ++n) BOOST_CHECK_EQUAL(db_many.GetCoin(outpoint(1, n)).has_value(), n >= PER_HEIGHT / 2);
    BOOST_CHECK(db_many.GetCoin(outpoint(2, 0)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(4, 0)).has_value());

    // And the rest, written by each store the way it writes: the same again.
    store_one.Sync();
    store_many.Sync();
    check_equal();
    BOOST_CHECK(db_many.GetBestBlock() == hash(6));
    BOOST_CHECK(db_many.GetCoin(outpoint(6, 1)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(2, 0)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(4, 0)).has_value());
    store_many.SanityCheck();
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
    Create(store, b2, new_coin, RandomCoin(m_rng, 2), false);
    BOOST_CHECK(store.ApplySpend(b2, old_coin));
    // Readers: the create is invisible, the old coin still there.
    BOOST_CHECK(!store.HaveCoin(new_coin));
    BOOST_CHECK(store.HaveCoin(old_coin));
    // Appliers (block 3): the create is visible, the old coin spent.
    BOOST_CHECK(store.LookupFor(b3, new_coin).has_value());
    BOOST_CHECK(!store.LookupFor(b3, old_coin).has_value());
    // Block 3 spending old_coin too: block 2 is lower, block 3 is the offender.
    BOOST_CHECK(!store.ApplySpend(b3, old_coin));
    BOOST_CHECK_EQUAL(store.ConflictCount(b3), 1U);
    BOOST_CHECK_EQUAL(store.ConflictCount(b2), 0U);
    // Nothing of this is writable yet.
    BOOST_CHECK(!store.BeginWriteOut(1, 1, uint256::ONE, false));
    store.SanityCheck();

    // Confirm block 2: readers now see the create and the spend; the entries are dirty and the run's to take.
    store.ConfirmBlock(b2);
    BOOST_CHECK(store.HaveCoin(new_coin));
    BOOST_CHECK(!store.HaveCoin(old_coin));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 2U); // new_coin to write, old_coin's erase
    auto run{WriteOut(store, 2, 2, uint256{2})};
    BOOST_CHECK_EQUAL(run->count, 2U);
    BOOST_CHECK(db.PeekCoin(new_coin).has_value());
    BOOST_CHECK(!db.PeekCoin(old_coin).has_value());

    // Block 3 (the offender) is unwound: nothing of it remains. A fresh block 4 spends new_coin and is
    // then unwound: the coin is back for everyone.
    store.UnwindBlock(b3);
    const auto b4{store.BeginBlock(4)};
    BOOST_CHECK(store.ApplySpend(b4, new_coin));
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
    BOOST_CHECK(store.ApplySpend(b6, new_coin));
    BOOST_CHECK(store.ApplySpend(b5, new_coin));
    BOOST_CHECK_EQUAL(store.ConflictCount(b6), 1U);
    BOOST_CHECK_EQUAL(store.ConflictCount(b5), 0U);
    store.ConfirmBlock(b5);
    BOOST_CHECK(!store.HaveCoin(new_coin));
    store.UnwindBlock(b6);
    store.SanityCheck();
    // A coin created and spent inside the window leaves no trace once confirmed.
    const auto b7{store.BeginBlock(7)};
    const COutPoint ephemeral{Txid::FromUint256(uint256{12}), 0};
    Create(store, b7, ephemeral, RandomCoin(m_rng, 7), false);
    BOOST_CHECK(store.ApplySpend(b7, ephemeral));
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
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false); // confirmed, dirty, tagged 1
    store.AddCoin(b, RandomCoin(m_rng, 1), false);

    const auto b2{store.BeginBlock(2)};
    BOOST_CHECK(store.ApplySpend(b2, a)); // a is held by block 2 now, still unspent for readers
    BOOST_CHECK(store.HaveCoin(a));

    auto run{WriteOut(store, /*bound_height=*/1, /*head_height=*/1, uint256{1})};
    BOOST_CHECK_EQUAL(base.m_writes.size(), 2U); // a and b, both as coins
    for (const auto& w : base.m_writes) BOOST_CHECK(w.coin.has_value());
    BOOST_CHECK_EQUAL(run->settled_height, 1);
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
    BOOST_CHECK(store.ApplySpend(b3c, c));
    base.m_in_flight = [&] {
        store.ConfirmBlock(b3c);
        store.SetBestBlock(uint256{3}, 3);
    };
    auto run_c{WriteOut(store, 2, 2, uint256{2})};
    BOOST_CHECK_EQUAL(run_c->count, 1U);
    BOOST_CHECK(db.HaveCoin(c));                   // the run wrote the coin
    BOOST_CHECK(!store.HaveCoin(c));               // spent in the tier
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);  // the erase is still owed
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(c));

    // The same with the block unwound instead: the coin stays, clean, as the database has it.
    const auto b3{store.BeginBlock(3)};
    BOOST_CHECK(store.ApplySpend(b3, b));
    BOOST_CHECK(!store.BeginWriteOut(2, 2, uint256{2}, false)); // b was written already and is clean: nothing to carry
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
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);

    // An applied block re-creates x (a duplicate, as BIP30 allowed once), then is unwound.
    const auto b3{store.BeginBlock(3)};
    Create(store, b3, x, RandomCoin(m_rng, 3), false);
    BOOST_CHECK(!store.HaveCoin(x));          // readers: still spent
    BOOST_CHECK(store.LookupFor(b3, x).has_value());
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2); // the owed erase keeps its tag
    store.UnwindBlock(b3);
    BOOST_CHECK(!store.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);

    // Applied again, with a final run while the block is undecided: the erase lands, the block's coin stays
    // the block's, and nothing is owed when the block is unwound.
    const auto b3b{store.BeginBlock(3)};
    Create(store, b3b, x, RandomCoin(m_rng, 3), false);
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
    Create(store, b6, x, RandomCoin(m_rng, 6), false);
    store.ConfirmBlock(b6);
    store.SetBestBlock(uint256{6}, 6);
    BOOST_CHECK(store.HaveCoin(x));
    // The erase still owed keeps its tag, the spend at 5, under the coin made at 6.
    BOOST_CHECK_EQUAL(*OldestDirty(store), 5);
    store.Sync();
    const auto coin{db.PeekCoin(x)};
    BOOST_REQUIRE(coin.has_value());
    BOOST_CHECK_EQUAL(coin->nHeight, 6U);
    store.SanityCheck();
}

//! The tier's byte count stays exact for coins whose scripts are on the heap, through the two paths that
//! empty an entry and keep it: a spend with no taker for the coin, and the unwind of a create that went over
//! an erase still owed. (An emptied script gives its memory back, CScript::clear().)
BOOST_AUTO_TEST_CASE(emptied_entries_count_no_script_bytes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const auto big{[](int height) { return Coin{CTxOut{CAmount{1000}, CScript{} << std::vector<unsigned char>(100, 0x42)}, height, /*fCoinBaseIn=*/false}; }};
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(x, big(1), false);
    store.Sync(); // x is in the database
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(x)); // no taker: the entry stays, spent and dirty
    store.SanityCheck();
    const auto b3{store.BeginBlock(3)};
    Create(store, b3, x, big(3), false);
    store.UnwindBlock(b3); // the erase is owed still: the entry is emptied again
    BOOST_CHECK(!store.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    store.SanityCheck();
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(x));
    store.SanityCheck();
}

//! Blocks the database holds are disconnected and connected again, past the written head. The erase the
//! disconnect left is tagged with the fork height and the coin is made again above it: a run that stopped
//! between the two would write the erase under a marker whose replay starts at the written head, where the
//! coin is taken to be there. Such a run writes everything.
BOOST_AUTO_TEST_CASE(run_after_a_disconnect_below_the_written_head_is_final)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.SetBestBlock(uint256{2}, 2);
    store.AddCoin(x, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    store.Sync(); // the database is at block 3, with x
    BOOST_CHECK(db.HaveCoin(x));

    // Blocks 3 and 2 are disconnected: the erase of x is a change of the fork height.
    store.SetChangeHeight(1);
    BOOST_CHECK(store.SpendCoin(x));
    store.SetBestBlock(uint256{1});
    store.SetTierHeight(1);
    // The same blocks are connected again, block 2 as a window block, and two more.
    const auto b2{store.BeginBlock(2)};
    Create(store, b2, x, RandomCoin(m_rng, 2), false);
    store.ConfirmBlock(b2);
    for (uint8_t height{2}; height <= 5; ++height) store.SetBestBlock(uint256{height}, height);

    // A run asked to stop at block 1 writes everything: the database keeps x.
    auto run{WriteOut(store, /*bound_height=*/1, /*head_height=*/5, uint256{5})};
    BOOST_CHECK(run->final);
    for (const auto& write : base.m_writes) BOOST_CHECK(write.outpoint != x || write.coin.has_value());
    BOOST_CHECK(db.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 5);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    store.SanityCheck();

    // With that run begun, a later one stops at its bound again.
    store.SetBestBlock(uint256{6}, 6);
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{11}), 0}, RandomCoin(m_rng, 6), false);
    store.SetBestBlock(uint256{7}, 7);
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{12}), 0}, RandomCoin(m_rng, 7), false);
    store.SetBestBlock(uint256{8}, 8);
    run = WriteOut(store, /*bound_height=*/6, /*head_height=*/8, uint256{8});
    BOOST_CHECK(!run->final);
    BOOST_CHECK_EQUAL(run->count, 1U);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 6);
    store.SanityCheck();
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
    Create(store, b3, dup, RandomCoin(m_rng, 3), false); // block 3 first: nothing of block 2 is visible to it
    BOOST_CHECK_EQUAL(store.ConflictCount(b3), 0U);
    Create(store, b2, dup, RandomCoin(m_rng, 2), false); // block 2 creates the same outpoint
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
    BOOST_CHECK(store.ApplySpend(lower, b));
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

    // A spend whose entry is not in the tier (here evicted; in the node also a coin that a block at the tip
    // read from the database, which does not enter it): the batch reads the base for it, one by one.
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

//! A block that spends a confirmed coin and then repeats the transaction that created it (invalid: the repeat's
//! inputs are spent) puts the coin back in its view, over the spend. Applied, that create meets the coin as a
//! conflict and changes nothing, so the block's unwind leaves the coin as it was. Only a coinbase may overwrite
//! an unspent coin (the chain's two BIP30 duplicates): it does, and an unwind puts the first coin back.
BOOST_AUTO_TEST_CASE(block_apply_view_repeat_over_a_confirmed_coin)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint t{Txid::FromUint256(uint256{10}), 0};
    const COutPoint cb{Txid::FromUint256(uint256{11}), 0};
    const Coin t_coin{CTxOut{CAmount{1000}, CScript{} << OP_TRUE}, 1, /*fCoinBaseIn=*/false};
    const Coin cb_coin{CTxOut{CAmount{5000}, CScript{} << OP_TRUE}, 1, /*fCoinBaseIn=*/true};
    store.AddCoin(t, Coin{t_coin}, false); // confirmed, dirty: the database does not have it yet
    store.AddCoin(cb, Coin{cb_coin}, false);
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    // The spend, then the repeat's create over it.
    view.BeginBlock(2);
    BOOST_CHECK(view.SpendCoin(t));
    view.AddCoin(t, Coin{CTxOut{CAmount{1000}, CScript{} << OP_TRUE}, 2, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
    view.ApplyChanges();
    BOOST_CHECK_EQUAL(view.Conflicts(), 1U);
    view.Unwind();
    const auto kept{store.PeekCoin(t)};
    BOOST_REQUIRE(kept);
    BOOST_CHECK(kept->out == t_coin.out && kept->nHeight == t_coin.nHeight && !kept->IsCoinBase());
    store.SanityCheck();

    // A coinbase over a confirmed coinbase: applied, then unwound, the first coin is back.
    view.BeginBlock(2);
    view.AddCoin(cb, Coin{CTxOut{CAmount{6000}, CScript{} << OP_TRUE}, 2, /*fCoinBaseIn=*/true}, /*possible_overwrite=*/true);
    view.ApplyChanges();
    BOOST_CHECK_EQUAL(view.Conflicts(), 0U);
    view.Unwind();
    const auto back{store.PeekCoin(cb)};
    BOOST_REQUIRE(back);
    BOOST_CHECK(back->out == cb_coin.out && back->nHeight == cb_coin.nHeight && back->IsCoinBase());
    store.SanityCheck();

    // Confirmed, the second coinbase stands: the first is overwritten for good.
    view.BeginBlock(2);
    view.AddCoin(cb, Coin{CTxOut{CAmount{6000}, CScript{} << OP_TRUE}, 2, /*fCoinBaseIn=*/true}, /*possible_overwrite=*/true);
    view.SetBestBlock(uint256{2});
    view.Confirm();
    const auto second{store.PeekCoin(cb)};
    BOOST_REQUIRE(second);
    BOOST_CHECK_EQUAL(second->out.nValue, CAmount{6000});
    store.SanityCheck();
}

//! A coinbase that overwrites a confirmed coin the database does not have yet (the chain's two BIP30
//! duplicates) keeps that coin for its block's unwind. A run or a flush while the block is open writes the
//! kept coin, the outpoint's value at the run's head, not an erase; the unwind puts the coin back after the
//! run too; and a lower block that would take such a create over fails instead, unable to see the coin
//! beneath it.
BOOST_AUTO_TEST_CASE(coinbase_overwrite_keeps_its_coin_through_a_run)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const Coin first{CTxOut{CAmount{5000}, CScript{} << OP_TRUE}, 1, /*fCoinBaseIn=*/true};
    const auto is_first{[&](const std::optional<Coin>& coin) {
        return coin && coin->out == first.out && coin->nHeight == first.nHeight && coin->IsCoinBase();
    }};
    const auto duplicate{[](int height) { return Coin{CTxOut{CAmount{6000}, CScript{} << OP_TRUE}, height, /*fCoinBaseIn=*/true}; }};

    for (const bool flush : {false, true}) {
        const COutPoint cb{Txid::FromUint256(uint256{11}), flush ? 1U : 0U};
        store.AddCoin(cb, Coin{first}, /*possible_overwrite=*/true); // confirmed, dirty: not in the database
        BOOST_REQUIRE(!db.HaveCoin(cb));
        const auto b3{store.BeginBlock(3)};
        Create(store, b3, cb, duplicate(3), /*possible_overwrite=*/true);
        BOOST_CHECK_EQUAL(store.ConflictCount(b3), 0U);
        if (flush) {
            store.Flush();
        } else {
            store.Sync();
        }
        BOOST_CHECK_MESSAGE(is_first(db.PeekCoin(cb)), "the database must hold the kept coin, not an erase");
        store.UnwindBlock(b3);
        BOOST_CHECK(is_first(store.PeekCoin(cb)));
        store.Sync();
        BOOST_CHECK(is_first(db.PeekCoin(cb)));
        store.SanityCheck();
    }

    // The lower block's create would take over the higher block's, which kept a coin: it fails.
    const COutPoint cb{Txid::FromUint256(uint256{11}), 2};
    store.AddCoin(cb, Coin{first}, /*possible_overwrite=*/true);
    const auto b4{store.BeginBlock(4)};
    const auto b5{store.BeginBlock(5)};
    Create(store, b5, cb, duplicate(5), /*possible_overwrite=*/true);
    Create(store, b4, cb, duplicate(4), /*possible_overwrite=*/true);
    BOOST_CHECK_EQUAL(store.ConflictCount(b5), 0U);
    BOOST_CHECK_EQUAL(store.ConflictCount(b4), 1U);
    store.UnwindBlock(b5);
    store.UnwindBlock(b4);
    BOOST_CHECK(is_first(store.PeekCoin(cb)));
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
        BOOST_CHECK(store.ApplySpend(b2, coin));
        store.ConfirmBlock(b2);
        store.SetBestBlock(uint256{2}, 2);
        store.Sync();
        BOOST_CHECK(!db.PeekCoin(coin).has_value());
    };
    BOOST_CHECK(!store.ApplySpend(b3, coin));
    BOOST_CHECK(!base.m_overtake);
    BOOST_CHECK_EQUAL(store.ConflictCount(b3), 1U);
    store.UnwindBlock(b3);
    store.SanityCheck();
}

//! The map nodes one thread frees, more than a thread keeps, serve another thread's insertions, and the
//! coins that land in them are whole.
BOOST_AUTO_TEST_CASE(freed_nodes_serve_another_thread)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    constexpr uint32_t COUNT{5000};
    const auto outpoint{[](uint8_t round, uint32_t i) { return COutPoint{Txid::FromUint256(uint256{round}), i}; }};
    const auto add{[&](uint8_t round) {
        for (uint32_t i{0}; i < COUNT; ++i) {
            store.AddCoin(outpoint(round, i), Coin{CTxOut{CAmount{i} + round, CScript{} << OP_TRUE}, 1, false}, false);
        }
    }};
    std::thread{add, uint8_t{1}}.join();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    // Fresh coins: each spend frees its node, two full lists stay with the thread and the rest change hands.
    store.SetChangeHeight(2);
    uint32_t spent{0};
    std::thread{[&] {
        for (uint32_t i{0}; i < COUNT; ++i) spent += store.SpendCoin(outpoint(1, i));
    }}.join();
    BOOST_CHECK_EQUAL(spent, COUNT);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    std::thread{add, uint8_t{2}}.join();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    for (uint32_t i{0}; i < COUNT; ++i) {
        BOOST_CHECK(!store.PeekCoin(outpoint(1, i)));
        const std::optional<Coin> coin{store.PeekCoin(outpoint(2, i))};
        BOOST_REQUIRE(coin);
        BOOST_CHECK_EQUAL(coin->out.nValue, CAmount{i} + 2);
    }
    // Many threads that each end with nodes on their lists: the nodes come back, and no region is carved
    // for the next thread.
    const size_t regions{CoinsStore::NodeRegionBytes()};
    for (uint8_t round{3}; round < 203; ++round) {
        std::thread{[&] {
            for (uint32_t i{0}; i < COUNT; ++i) store.SpendCoin(outpoint(static_cast<uint8_t>(round - 1), i));
        }}.join();
        BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U);
        std::thread{add, round}.join();
    }
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    BOOST_CHECK_EQUAL(CoinsStore::NodeRegionBytes(), regions);
}

//! Where outpoints made twice are checked for (BIP30), a higher block's spend of a coin the database never
//! held leaves a marker: a lower block still sees the coin, and its create of the outpoint is a conflict.
//! Unwound, the coin is back as it was; confirmed, the marker is freed: the database never had the coin.
BOOST_AUTO_TEST_CASE(fresh_spend_marked_for_lower_create)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint coin{Txid::FromUint256(uint256{10}), 0};
    const COutPoint plain{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(coin, RandomCoin(m_rng, 1), false);
    store.AddCoin(plain, RandomCoin(m_rng, 1), false);
    BOOST_REQUIRE(!db.PeekCoin(coin).has_value()); // never written
    const size_t dirty{store.GetDirtyCount()};

    // Without the mark the entry is erased, and a lower applier finds nothing: enough where no block
    // makes an outpoint twice.
    {
        const auto lower{store.BeginBlock(2)};
        const auto higher{store.BeginBlock(3)};
        BOOST_CHECK(store.ApplySpend(higher, plain));
        BOOST_CHECK(!store.HaveCoinInCache(plain));
        BOOST_CHECK(!store.PeekCoinFor(lower, plain));
        store.UnwindBlock(higher);
        store.UnwindBlock(lower);
        BOOST_CHECK(store.HaveCoin(plain));
    }

    const auto lower{store.BeginBlock(2, /*mark_fresh_spends=*/true)};
    const auto higher{store.BeginBlock(3, /*mark_fresh_spends=*/true)};
    BOOST_CHECK(store.ApplySpend(higher, coin));
    BOOST_CHECK(!store.PeekCoinFor(higher, coin));
    BOOST_CHECK(store.HaveCoin(coin));
    // The lower block's check for the outpoint finds the coin, unspent at its height.
    BOOST_CHECK(store.PeekCoinFor(lower, coin));
    // And its create, had it not looked, is a conflict, not an overwrite and not a defect.
    Create(store, lower, coin, RandomCoin(m_rng, 2), /*possible_overwrite=*/false);
    BOOST_CHECK_EQUAL(store.ConflictCount(lower), 1U);
    Create(store, lower, coin, RandomCoin(m_rng, 2), /*possible_overwrite=*/true);
    BOOST_CHECK_EQUAL(store.ConflictCount(lower), 2U);
    BOOST_CHECK_EQUAL(store.ConflictCount(higher), 0U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), dirty);
    store.SanityCheck();

    // Both are taken back: the coin is there again, still to be written.
    store.UnwindBlock(higher);
    store.UnwindBlock(lower);
    BOOST_CHECK(store.HaveCoin(coin));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), dirty);
    store.SanityCheck();

    // The spend alone, confirmed: the marker is freed (ConfirmBlock()), and nothing is written for a key the
    // database does not have.
    const auto again{store.BeginBlock(2, /*mark_fresh_spends=*/true)};
    BOOST_CHECK(store.ApplySpend(again, coin));
    store.ConfirmBlock(again);
    store.SetBestBlock(uint256{2}, 2);
    BOOST_CHECK(!store.HaveCoin(coin));
    store.Sync();
    BOOST_CHECK(!db.PeekCoin(coin).has_value());
    BOOST_CHECK(db.PeekCoin(plain).has_value());
    BOOST_CHECK(!store.HaveCoinInCache(coin));
    store.SanityCheck();
}

//! A create that takes no coin to be at its outpoint and meets one the block sees is a conflict, counted
//! against the block and left for its connection alone to judge, whether the coin is confirmed or a lower
//! open block's. Where the block was checked to overwrite nothing, that holds for a create that may
//! overwrite as well: a lower block can put its coin there after the check.
BOOST_AUTO_TEST_CASE(create_over_a_visible_coin_is_a_conflict)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint confirmed{Txid::FromUint256(uint256{10}), 0};
    const COutPoint open{Txid::FromUint256(uint256{11}), 0};
    const Coin first{RandomCoin(m_rng, 1)};
    store.AddCoin(confirmed, Coin{first}, false);

    {
        const auto lower{store.BeginBlock(2)};
        const auto higher{store.BeginBlock(3)};
        const Coin lower_coin{RandomCoin(m_rng, 2)};
        Create(store, lower, open, Coin{lower_coin}, /*possible_overwrite=*/false);
        BOOST_CHECK_EQUAL(store.ConflictCount(lower), 0U);
        // Over a confirmed coin and over the lower block's.
        Create(store, higher, confirmed, RandomCoin(m_rng, 3), /*possible_overwrite=*/false);
        BOOST_CHECK_EQUAL(store.ConflictCount(higher), 1U);
        Create(store, higher, open, RandomCoin(m_rng, 3), /*possible_overwrite=*/false);
        BOOST_CHECK_EQUAL(store.ConflictCount(higher), 2U);
        // Neither coin was touched.
        BOOST_CHECK(Assert(store.PeekCoinFor(higher, confirmed))->out == first.out);
        BOOST_CHECK(Assert(store.PeekCoinFor(higher, open))->out == lower_coin.out);
        store.SanityCheck();
        store.UnwindBlock(higher);
        store.UnwindBlock(lower);
        BOOST_CHECK(Assert(store.PeekCoin(confirmed))->out == first.out);
        BOOST_CHECK(!store.HaveCoin(open));
        store.SanityCheck();
    }

    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    int height{2};
    for (const bool must_be_new : {false, true}) {
        const COutPoint twice{Txid::FromUint256(must_be_new ? uint256{13} : uint256{12}), 0};
        BlockApplyView lower{store, pool, /*deterministic=*/true};
        BlockApplyView higher{store, pool, /*deterministic=*/true};
        lower.BeginBlock(height);
        higher.BeginBlock(height + 1);
        higher.SetCreatesMustBeNew(must_be_new);
        // The higher block looked and found nothing, as its check for overwritten coins does; its output
        // is a coinbase's, added without a look.
        BOOST_CHECK(!higher.HaveCoin(twice));
        Coin higher_coin{RandomCoin(m_rng, height + 1)};
        higher_coin.fCoinBase = true;
        higher.AddCoin(twice, Coin{higher_coin}, /*possible_overwrite=*/true);
        // The lower block makes the same outpoint and is confirmed in the meantime.
        const Coin lower_coin{RandomCoin(m_rng, height)};
        lower.AddCoin(twice, Coin{lower_coin}, /*possible_overwrite=*/true);
        lower.SetBestBlock(uint256{static_cast<uint8_t>(height)});
        lower.Confirm();
        store.SetBestBlock(uint256{static_cast<uint8_t>(height)}, height);
        higher.ApplyChanges();
        BOOST_CHECK_EQUAL(higher.Conflicts(), must_be_new ? 1U : 0U);
        BOOST_CHECK(Assert(store.PeekCoinFor(higher.Block(), twice))->out == (must_be_new ? lower_coin : higher_coin).out);
        higher.Unwind();
        // The lower block's coin is there again, whether it was overwritten or not.
        BOOST_CHECK(Assert(store.PeekCoin(twice))->out == lower_coin.out);
        store.SanityCheck();
        height += 2;
    }
}

//! A block validates the spend of a coin; lower blocks then spend that coin and make the outpoint again
//! (a transaction made twice). The coin the block would spend at its apply is not the one it validated.
BOOST_AUTO_TEST_CASE(spend_of_another_coin_is_a_conflict)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint twice{Txid::FromUint256(uint256{10}), 0};
    const COutPoint once{Txid::FromUint256(uint256{11}), 0};
    Coin first{RandomCoin(m_rng, 1)};
    first.fCoinBase = true;
    store.AddCoin(twice, Coin{first}, false);
    store.AddCoin(once, RandomCoin(m_rng, 1), false);

    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView spender{store, pool, /*deterministic=*/true};
    BlockApplyView remaker{store, pool, /*deterministic=*/true};
    BlockApplyView higher{store, pool, /*deterministic=*/true};
    spender.BeginBlock(2);
    remaker.BeginBlock(3);
    higher.BeginBlock(4);
    // The higher block spends both coins as it found them.
    Coin spent;
    BOOST_CHECK(higher.SpendCoin(twice, &spent));
    BOOST_CHECK_EQUAL(spent.nHeight, 1U);
    BOOST_CHECK(higher.SpendCoin(once));
    // Below it, one coin is spent and its outpoint made again, with the same output.
    BOOST_CHECK(spender.SpendCoin(twice));
    spender.SetBestBlock(uint256{2});
    spender.Confirm();
    store.SetBestBlock(uint256{2}, 2);
    Coin second{first};
    second.nHeight = 3;
    remaker.AddCoin(twice, Coin{second}, /*possible_overwrite=*/false);
    remaker.SetBestBlock(uint256{3});
    remaker.Confirm();
    store.SetBestBlock(uint256{3}, 3);

    higher.ApplyChanges();
    // The spend of the coin made again is the conflict; the other spend is applied.
    BOOST_CHECK_EQUAL(higher.Conflicts(), 1U);
    BOOST_CHECK(!store.PeekCoinFor(higher.Block(), once));
    BOOST_CHECK_EQUAL(Assert(store.PeekCoinFor(higher.Block(), twice))->nHeight, 3U);
    higher.Unwind();
    BOOST_CHECK(store.HaveCoin(once));
    BOOST_CHECK_EQUAL(Assert(store.PeekCoin(twice))->nHeight, 3U);
    store.SanityCheck();

    // Connected again alone, the block reads the second coin and spends it.
    higher.BeginBlock(4);
    BOOST_CHECK(higher.SpendCoin(twice, &spent));
    BOOST_CHECK_EQUAL(spent.nHeight, 3U);
    higher.ApplyChanges();
    BOOST_CHECK_EQUAL(higher.Conflicts(), 0U);
    higher.SetBestBlock(uint256{4});
    higher.Confirm();
    store.SetBestBlock(uint256{4}, 4);
    BOOST_CHECK(!store.HaveCoin(twice));
    store.SanityCheck();
}

//! A block checked to make no outpoint that has a coin finds no entry at its apply. That is no proof once
//! changes confirmed since the block was opened have been written and may have left the tier: the coin
//! of a lower block may be in the database alone.
BOOST_AUTO_TEST_CASE(create_that_must_be_new_with_the_base_unseen)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    store.InitWrittenHead(uint256::ONE, 1);
    const COutPoint twice{Txid::FromUint256(uint256{10}), 0};

    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    for (const bool written : {false, true}) {
        const int height{written ? 4 : 2};
        BlockApplyView lower{store, pool, /*deterministic=*/true};
        BlockApplyView higher{store, pool, /*deterministic=*/true};
        lower.BeginBlock(height);
        higher.BeginBlock(height + 1);
        higher.SetCreatesMustBeNew(true);
        BOOST_CHECK(!higher.HaveCoin(twice));
        const COutPoint other{Txid::FromUint256(uint256{static_cast<uint8_t>(20 + height)}), 0};
        higher.AddCoin(written ? twice : other, RandomCoin(m_rng, height + 1), /*possible_overwrite=*/true);
        // The lower block is confirmed, and with `written` its coin is written and dropped from the tier.
        if (written) lower.AddCoin(twice, RandomCoin(m_rng, height), /*possible_overwrite=*/true);
        lower.SetBestBlock(uint256{static_cast<uint8_t>(height)});
        lower.Confirm();
        store.SetBestBlock(uint256{static_cast<uint8_t>(height)}, height);
        if (written) {
            WriteOut(store, height, height, uint256{static_cast<uint8_t>(height)});
            store.EvictClean(0);
            BOOST_CHECK(!store.HaveCoinInCache(twice));
            BOOST_CHECK(db.HaveCoin(twice));
        }
        higher.ApplyChanges();
        BOOST_CHECK_EQUAL(higher.Conflicts(), written ? 1U : 0U);
        higher.Unwind();
        store.SanityCheck();
    }
}

//! A disconnect that spends a coin made again over an owed erase keeps the erase's height: the outpoint was
//! spent at 2 and made again at 4, and the disconnect back to 3 spends it at the fork height. A run bounded at
//! 2 must carry the erase, or settle below it; settled at 2 with the old coin still written, the database
//! would hold a coin no replay from there takes away.
BOOST_AUTO_TEST_CASE(disconnect_keeps_the_owed_erase_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{30}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync(); // x is in the database
    BOOST_REQUIRE(db.HaveCoin(x));
    store.SetBestBlock(uint256{2}, 2);
    BOOST_REQUIRE(store.SpendCoin(x)); // the erase is owed, as of 2
    // Another change at 2, so that a run bounded there has something to write either way.
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{31}), 0}, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    const auto b4{store.BeginBlock(4)};
    Create(store, b4, x, RandomCoin(m_rng, 4), false); // made again (a duplicate), over the owed erase
    store.ConfirmBlock(b4);
    store.SetBestBlock(uint256{4}, 4);

    // Block 4 disconnected towards the fork at 3: its coin is spent at the fork height.
    store.SetChangeHeight(3);
    BOOST_REQUIRE(store.SpendCoin(x));
    store.SetBestBlock(uint256{3});
    store.SetTierHeight(3);

    WriteOut(store, /*bound_height=*/2, /*head_height=*/3, uint256{3});
    BOOST_CHECK_MESSAGE(!db.HaveCoin(x) || store.WrittenHeight() < 2,
                        "the database settled at " << store.WrittenHeight() << " with a coin spent at 2");
    store.SanityCheck();
}

//! The same for a block's spend: the outpoint was spent at 2 and made again at 4, over the owed erase, and
//! block 5 spends that coin. While block 5 is open its record carries the erase's height; once confirmed,
//! the entry has to. A run bounded at 2 must carry the erase, or settle below it.
BOOST_AUTO_TEST_CASE(block_spend_keeps_the_owed_erase_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{30}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync(); // x is in the database
    BOOST_REQUIRE(db.HaveCoin(x));
    store.SetBestBlock(uint256{2}, 2);
    BOOST_REQUIRE(store.SpendCoin(x)); // the erase is owed, as of 2
    // Another change at 2, so that a run bounded there has something to write either way.
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{31}), 0}, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    const auto b4{store.BeginBlock(4)};
    Create(store, b4, x, RandomCoin(m_rng, 4), false); // made again (a duplicate), over the owed erase
    store.ConfirmBlock(b4);
    store.SetBestBlock(uint256{4}, 4);
    const auto b5{store.BeginBlock(5)};
    BOOST_REQUIRE(store.ApplySpend(b5, x)); // spent by a block, the erase of 2 still owed
    store.ConfirmBlock(b5);
    store.SetBestBlock(uint256{5}, 5);

    WriteOut(store, /*bound_height=*/2, /*head_height=*/5, uint256{5});
    BOOST_CHECK_MESSAGE(!db.HaveCoin(x) || store.WrittenHeight() < 2,
                        "the database settled at " << store.WrittenHeight() << " with a coin spent at 2");
    store.SanityCheck();
}

//! The marker of a never-written coin is no erase: the run after its block's confirmation drops it. If a
//! run wrote the coin while the block was open, the marker is the erase of it.
BOOST_AUTO_TEST_CASE(fresh_spend_marker_not_written)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint dropped{Txid::FromUint256(uint256{10}), 0};
    const COutPoint written{Txid::FromUint256(uint256{11}), 0};
    const COutPoint kept{Txid::FromUint256(uint256{12}), 0};
    store.AddCoin(dropped, RandomCoin(m_rng, 1), false);
    store.AddCoin(kept, RandomCoin(m_rng, 1), false);

    // Spent and confirmed before any run: the marker is in the map while the block is open and gone
    // once it is confirmed, the coin beside it is the one write.
    auto block{store.BeginBlock(2, /*mark_fresh_spends=*/true)};
    BOOST_CHECK(store.ApplySpend(block, dropped));
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 2U);
    store.ConfirmBlock(block);
    store.SetBestBlock(uint256{2}, 2);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 1U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    store.SanityCheck();
    WriteOut(store, 2, 2, uint256{2});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U); // only the coin kept
    BOOST_CHECK(base.m_writes[0].coin.has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 1U);
    BOOST_CHECK(!store.HaveCoin(dropped));
    store.SanityCheck();

    // A run while the spending block is open writes the coin from the block's record: the marker is an
    // erase from then on.
    store.SetChangeHeight(2);
    store.AddCoin(written, RandomCoin(m_rng, 2), false);
    block = store.BeginBlock(3, /*mark_fresh_spends=*/true);
    BOOST_CHECK(store.ApplySpend(block, written));
    WriteOut(store, 2, 2, uint256{2});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(base.m_writes[0].coin.has_value());
    BOOST_CHECK(db.PeekCoin(written).has_value());
    store.ConfirmBlock(block);
    store.SetBestBlock(uint256{3}, 3);
    store.SanityCheck();
    WriteOut(store, 3, 3, uint256{3});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(!base.m_writes[0].coin.has_value());
    BOOST_CHECK(!db.PeekCoin(written).has_value());
    BOOST_CHECK(db.PeekCoin(kept).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    store.SanityCheck();
}

//! A lower block that takes over a higher block's spend of a never-written coin (the higher one is in conflict
//! and unwound) keeps the marker's word: once the lower block is confirmed the run writes no erase for it.
BOOST_AUTO_TEST_CASE(taken_over_spend_keeps_the_marker_unwritten)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint coin{Txid::FromUint256(uint256{20}), 0};
    const COutPoint kept{Txid::FromUint256(uint256{21}), 0};
    store.AddCoin(coin, RandomCoin(m_rng, 1), false);
    store.AddCoin(kept, RandomCoin(m_rng, 1), false);

    auto high{store.BeginBlock(3, /*mark_fresh_spends=*/true)};
    BOOST_REQUIRE(store.ApplySpend(high, coin));
    auto low{store.BeginBlock(2, /*mark_fresh_spends=*/true)};
    BOOST_REQUIRE(store.ApplySpend(low, coin));
    BOOST_CHECK_EQUAL(store.ConflictCount(high), 1U);
    BOOST_CHECK_EQUAL(store.ConflictCount(low), 0U);
    store.UnwindBlock(high);
    store.ConfirmBlock(low);
    store.SetBestBlock(uint256{2}, 2);
    store.SanityCheck();

    WriteOut(store, 2, 2, uint256{2});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U); // the coin kept, and no erase of the coin never written
    BOOST_CHECK(base.m_writes[0].outpoint == kept);
    BOOST_CHECK(base.m_writes[0].coin.has_value());
    BOOST_CHECK(!store.HaveCoin(coin));
}

//! A base that counts its reads.
class ReadCountingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    mutable int m_reads{0};
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        ++m_reads;
        return CCoinsViewBacked::GetCoin(outpoint);
    }
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override
    {
        ++m_reads;
        return CCoinsViewBacked::PeekCoin(outpoint);
    }
};

//! A window block's view reads a coin that only the database holds once: the read brings the coin into the
//! tier, and the spend finds it there at the apply. Unwound, the block leaves the coin as the database has
//! it. Without the switch (a block at the tip) the read leaves nothing behind and the apply reads again.
BOOST_AUTO_TEST_CASE(block_apply_view_reads_a_database_coin_once)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    ReadCountingBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint spent_coin{Txid::FromUint256(uint256{10}), 0};
    const COutPoint kept_coin{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(spent_coin, RandomCoin(m_rng, 1), false);
    store.AddCoin(kept_coin, RandomCoin(m_rng, 1), false);
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U); // both coins are in the database only
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    base.m_reads = 0;
    view.BeginBlock(2);
    view.SetKeepBaseReads(true);
    BOOST_CHECK(view.SpendCoin(spent_coin));
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    BOOST_CHECK(store.HaveCoinInCache(spent_coin)); // clean, as the database has it
    view.SetBestBlock(uint256{2});
    view.Confirm();
    BOOST_CHECK_EQUAL(base.m_reads, 1); // the apply found the entry
    BOOST_CHECK_EQUAL(view.Conflicts(), 0U);
    BOOST_CHECK(!store.HaveCoin(spent_coin));
    store.SanityCheck();
    store.Sync();
    BOOST_CHECK(!db.PeekCoin(spent_coin).has_value()); // the erase was owed and is written

    base.m_reads = 0;
    view.BeginBlock(3);
    view.SetKeepBaseReads(true);
    BOOST_CHECK(view.SpendCoin(kept_coin));
    view.ApplyChanges();
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    view.Unwind();
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    BOOST_CHECK(store.HaveCoin(kept_coin));
    BOOST_CHECK_EQUAL(store.OpenBlocks(), 0U);
    store.SanityCheck();
    store.Sync();
    BOOST_CHECK(db.PeekCoin(kept_coin).has_value());

    // The switch went with the block: the next one reads as at the tip.
    store.EvictClean(0);
    BOOST_REQUIRE(!store.HaveCoinInCache(kept_coin));
    base.m_reads = 0;
    view.BeginBlock(3);
    BOOST_CHECK(view.SpendCoin(kept_coin));
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    BOOST_CHECK(!store.HaveCoinInCache(kept_coin));
    view.ApplyChanges();
    BOOST_CHECK_EQUAL(base.m_reads, 2);
    view.Unwind();
    BOOST_CHECK(store.HaveCoin(kept_coin));
    store.SanityCheck();
}

//! A window block announces its transactions; until its changes are applied a higher block is told that
//! their coins are pending, and no other block is. The mark goes with the apply, with the unwind, and
//! does not pass to the next block in the slot.
BOOST_AUTO_TEST_CASE(pending_creates_are_announced_until_applied)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    util::NotNull pool{std::make_shared<ThreadPool>("test")};
    BlockApplyView lower{store, pool, /*deterministic=*/true};
    BlockApplyView higher{store, pool, /*deterministic=*/true};
    const auto make_block{[](uint32_t salt) {
        CBlock block;
        for (uint32_t i{0}; i < 3; ++i) {
            CMutableTransaction mtx;
            mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(salt)}), i});
            mtx.vout.emplace_back(1000 + i, CScript{} << OP_TRUE);
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        return block;
    }};
    const CBlock block_low{make_block(20)};
    const CBlock block_high{make_block(21)};
    const Txid low_txid{block_low.vtx[1]->GetHash()};
    const Txid high_txid{block_high.vtx[1]->GetHash()};

    lower.BeginBlock(2);
    higher.BeginBlock(3);
    BOOST_CHECK(!higher.CreatesPendingBelow(low_txid)); // nothing announced
    lower.AnnounceCreates(block_low);
    higher.AnnounceCreates(block_high);
    for (const auto& tx : block_low.vtx) BOOST_CHECK(higher.CreatesPendingBelow(tx->GetHash()));
    BOOST_CHECK(!lower.CreatesPendingBelow(low_txid));   // its own
    BOOST_CHECK(!higher.CreatesPendingBelow(high_txid)); // its own
    BOOST_CHECK(!lower.CreatesPendingBelow(high_txid));  // a higher block's
    BOOST_CHECK(!store.CreatesPendingBelow(CoinsStore::NO_BLOCK, low_txid));
    BOOST_CHECK(!higher.CreatesPendingBelow(Txid::FromUint256(uint256{22})));

    // Applied: the coin is in the store, and is looked up there.
    AddCoins(lower, *block_low.vtx[1], 2);
    lower.ApplyChanges();
    BOOST_CHECK(!higher.CreatesPendingBelow(low_txid));
    BOOST_CHECK(higher.HaveCoin(COutPoint{low_txid, 0}));
    higher.Unwind();
    lower.Unwind();

    // Unwound before it applied: nothing is pending, and the next block in the slot starts without the mark.
    lower.BeginBlock(2);
    higher.BeginBlock(3);
    lower.AnnounceCreates(block_low);
    BOOST_CHECK(higher.CreatesPendingBelow(low_txid));
    higher.Unwind();
    lower.Unwind();
    lower.BeginBlock(2);
    higher.BeginBlock(3);
    BOOST_CHECK(!higher.CreatesPendingBelow(low_txid));
    // A block with nothing to apply is applied all the same.
    lower.AnnounceCreates(block_low);
    BOOST_CHECK(higher.CreatesPendingBelow(low_txid));
    lower.ApplyChanges();
    BOOST_CHECK(!higher.CreatesPendingBelow(low_txid));
    higher.Unwind();
    lower.Unwind();
    BOOST_CHECK_EQUAL(store.OpenBlocks(), 0U);
    store.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
