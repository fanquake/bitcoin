// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/solver.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/byte_ledger.h>
#include <util/check.h>
#include <util/string.h>
#include <util/threadpool.h>
#include <util/time.h>
#include <coinsstore.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CTxMemPool;

BOOST_FIXTURE_TEST_SUITE(validation_chainstate_tests, ChainTestingSetup)

//! Test resizing coins-related Chainstate caches during runtime.
//!
BOOST_AUTO_TEST_CASE(validation_chainstate_resize_caches)
{
    ChainstateManager& manager = *Assert(m_node.chainman);
    CTxMemPool& mempool = *Assert(m_node.mempool);
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool));
    c1.InitCoinsDB(
        /*cache_size_bytes=*/8_MiB, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(8_MiB));
    BOOST_REQUIRE(manager.LoadGenesisBlock()); // Need at least one block loaded to be able to flush caches

    // Add a coin to the in-memory cache, upsize once, then downsize.
    {
        LOCK(::cs_main);
        const auto outpoint = AddTestCoin(m_rng, c1.CoinsTip());

        // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
        // flush during ResizecoinsCaches() and will subsequently hit an assertion.
        c1.CoinsTip().SetBestBlock(m_rng.rand256());

        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            16_MiB, // upsizing the coinsview cache
            4_MiB // downsizing the coinsdb cache
        );

        // View should still have the coin cached, since we haven't destructed the cache on upsize.
        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            4_MiB, // downsizing the coinsview cache
            8_MiB // upsizing the coinsdb cache
        );

        // The view cache should be empty since we had to destruct to downsize.
        BOOST_CHECK(!c1.CoinsTip().HaveCoinInCache(outpoint));
    }
}

BOOST_FIXTURE_TEST_CASE(connect_tip_does_not_cache_inputs_on_failed_connect, TestChain100Setup)
{
    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};

    COutPoint outpoint;
    {
        LOCK(cs_main);
        outpoint = AddTestCoin(m_rng, chainstate.CoinsTip());
        chainstate.CoinsTip().Flush(/*reallocate_cache=*/false);
    }

    CMutableTransaction tx;
    tx.vin.emplace_back(outpoint);
    tx.vout.emplace_back(MAX_MONEY, CScript{} << OP_TRUE);

    const auto tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const CBlock block{CreateBlock({tx}, CScript{} << OP_TRUE)};
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(block), true, true, nullptr));

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip()->GetBlockHash()); // block rejected
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoinInCache(outpoint));    // input not cached
}

//! Test UpdateTip behavior for both active and background chainstates.
//!
//! When run on the background chainstate, UpdateTip should do a subset
//! of what it does for the active chainstate.
BOOST_FIXTURE_TEST_CASE(chainstate_update_tip, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const auto get_notify_tip{[&]() {
        LOCK(m_node.notifications->m_tip_block_mutex);
        BOOST_REQUIRE(m_node.notifications->TipBlock());
        return *m_node.notifications->TipBlock();
    }};
    uint256 curr_tip = get_notify_tip();

    // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
    // be found.
    mineBlocks(10);

    // After adding some blocks to the tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    // Grab block 1 from disk; we'll add it to the background chain later.
    std::shared_ptr<CBlock> pblockone = std::make_shared<CBlock>();
    {
        LOCK(::cs_main);
        chainman.m_blockman.ReadBlock(*pblockone, *chainman.ActiveChain()[1]);
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(
        this, NoMalleation, /*reset_chainstate=*/ true));

    // Ensure our active chain is the snapshot chainstate.
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));

    curr_tip = get_notify_tip();

    // Mine a new block on top of the activated snapshot chainstate.
    mineBlocks(1);  // Defined in TestChain100Setup.

    // After adding some blocks to the snapshot tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    curr_tip = get_notify_tip();

    Chainstate& background_cs{*Assert(WITH_LOCK(::cs_main, return chainman.HistoricalChainstate()))};

    // Append the first block to the background chain.
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    const CChainParams& chainparams = Params();
    bool newblock = false;

    // TODO: much of this is inlined from ProcessNewBlock(); just reuse PNB()
    // once it is changed to support multiple chainstates.
    {
        LOCK(::cs_main);
        bool checked = CheckBlock(*pblockone, state, chainparams.GetConsensus());
        BOOST_CHECK(checked);
        bool accepted = chainman.AcceptBlock(
            pblockone, state, &pindex, true, nullptr, &newblock, true);
        BOOST_CHECK(accepted);
    }

    // UpdateTip is called here
    bool block_added = background_cs.ActivateBestChain(state, pblockone);

    // Ensure tip is as expected
    BOOST_CHECK_EQUAL(background_cs.m_chain.Tip()->GetBlockHash(), pblockone->GetHash());

    // get_notify_tip() should be unchanged after adding a block to the background
    // validation chain.
    BOOST_CHECK(block_added);
    BOOST_CHECK_EQUAL(curr_tip, get_notify_tip());
}

//! A coin created at or below the height the coins database settled at and spent above it, before a run carried
//! it, cancels in the young tier: the database never holds it, although it is alive at that height. A
//! disconnect of the spend restores it. However the write-out runs after that are bounded, the database must
//! then hold the coin, or be settled below its creation, where a replay creates it again.
BOOST_FIXTURE_TEST_CASE(disconnect_restores_a_coin_the_database_never_held, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
    // A write-out run bounded at `bound` (unless it must be final), started as FlushStateToDisk() starts one and
    // waited for.
    const auto run_to{[&](int bound) {
        LOCK(::cs_main);
        BlockValidationState run_state;
        BOOST_REQUIRE(chainstate.StartWriteOutRun(run_state, bound, /*final=*/false));
        chainstate.SettleWriteOut();
    }};

    WITH_LOCK(::cs_main, chainstate.ForceFlushStateToDisk());
    const int base{WITH_LOCK(::cs_main, return chainstate.m_chain.Height())};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHeight()), base);

    // X is created in base+1 and spent in base+5.
    const CMutableTransaction create{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, script, 49 * COIN, /*submit=*/false)};
    CreateAndProcessBlock({create}, script);
    const COutPoint coin{create.GetHash(), 0};
    for (int i{0}; i < 3; ++i) CreateAndProcessBlock({}, script);
    const CMutableTransaction spend{CreateValidMempoolTransaction(MakeTransactionRef(create), 0, base + 1, coinbaseKey, script, 48 * COIN, /*submit=*/false)};
    const CBlock spend_block{CreateAndProcessBlock({spend}, script)};
    CreateAndProcessBlock({}, script);

    // A run bounded at base+2: X was created and spent inside the lag, so it cancels; the database settles
    // at base+2 without it.
    run_to(base + 2);
    {
        LOCK(::cs_main);
        BOOST_REQUIRE_EQUAL(chainstate.CoinsStoreTip().WrittenHeight(), base + 2);
        BOOST_REQUIRE(!chainstate.CoinsDB().HaveCoin(coin));
    }

    // The spend is disconnected (the fork is at base+4): X is alive again.
    CBlockIndex* const spend_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(spend_block.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, spend_index));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(coin)));

    // A run bounded between the settled height and the fork.
    run_to(base + 3);
    LOCK(::cs_main);
    chainman.m_blockman.SettleUndoWrites();
    BOOST_CHECK_MESSAGE(chainstate.CoinsDB().HaveCoin(coin) || chainstate.CoinsStoreTip().WrittenHeight() < base + 1,
                        "the database settled at " << chainstate.CoinsStoreTip().WrittenHeight() << " without the restored coin");
}

//! A disconnect writes the coins it restores only on the written head's own chain. With the written head on
//! another branch, a restored coin may come from a block that no chain the database can be replayed to holds:
//! written, it would stay in the database after a crash as a coin nothing created.
BOOST_FIXTURE_TEST_CASE(disconnect_writes_no_coin_of_another_branch, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};

    // The database is written at the tip, block T, two blocks up so that the fork can spend a coinbase.
    mineBlocks(2);
    WITH_LOCK(::cs_main, chainstate.ForceFlushStateToDisk());
    CBlockIndex* const tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHead()) == tip->GetBlockHash());

    // A fork from T-1: T' creates a coin Y that T'+1 spends.
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, tip));
    const CMutableTransaction create{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, script, 49 * COIN, /*submit=*/false)};
    const CBlock fork1{CreateAndProcessBlock({create}, script)};
    const COutPoint y{create.GetHash(), 0};
    const CMutableTransaction spend{CreateValidMempoolTransaction(MakeTransactionRef(create), 0, tip->nHeight, coinbaseKey, script, 48 * COIN, /*submit=*/false)};
    CreateAndProcessBlock({spend}, script);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), tip->nHeight + 1);
    BOOST_REQUIRE_MESSAGE(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHead()) == tip->GetBlockHash(),
                          "written at height " << WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHeight()));
    // T'+1 and T' are disconnected: Y comes back with T'+1's disconnect, then goes with T''s.
    CBlockIndex* const fork1_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(fork1.GetHash()))};
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, fork1_index));
    LOCK(::cs_main);
    chainman.m_blockman.SettleUndoWrites();
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(y));
    // The coins the next run writes before its marker: Y, created at the written height but on T''s branch,
    // is not among them.
    const auto restored{chainstate.CoinsStoreTip().TakeRestored()};
    BOOST_CHECK_MESSAGE(std::none_of(restored.begin(), restored.end(), [&](const CoinWrite& write) { return write.outpoint == y; }),
                        "a coin of a branch the written head is not on is to be written");
    BOOST_CHECK(!chainstate.CoinsDB().HaveCoin(y));
}

//! A block index write records a block whose undo write is still queued with the undo data it is getting, so
//! that a write-out run need not wait for the write under cs_main: the run commits the batch only behind the
//! write. The entry itself is left as it is until the write's completion records it.
BOOST_FIXTURE_TEST_CASE(index_batch_records_a_queued_undo_write, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};
    blockman.SettleUndoWrites();
    LOCK(::cs_main);
    CBlockIndex* tip{chainman.ActiveChain().Tip()};
    const uint32_t status{tip->nStatus};
    const uint64_t undo_pos{tip->nUndoPos};
    // No entry is left to write: the claim alone has the tip's written, as it must for a block whose
    // validity does not rise when it is connected (one stored again after its files were pruned).
    {
        auto written{blockman.PrepareBlockIndexBatch()};
        BOOST_REQUIRE(blockman.SyncBatchFiles(written));
        blockman.CommitBlockIndexBatch(written);
    }
    // The tip as a block just published has it: no undo data recorded, its write claimed and queued.
    tip->nStatus &= ~BLOCK_HAVE_UNDO;
    tip->nUndoPos = 0;
    BlockValidationState state;
    const auto claimed{blockman.ClaimUndoPos(state, *tip, /*undo_size=*/100)};
    BOOST_REQUIRE(claimed);
    auto batch{blockman.PrepareBlockIndexBatch()};
    BOOST_REQUIRE(blockman.SyncBatchFiles(batch));
    blockman.CommitBlockIndexBatch(batch);

    CDiskBlockIndex disk;
    BOOST_REQUIRE(blockman.m_block_tree_db->Read(std::make_pair(uint8_t{'b'}, tip->GetBlockHash()), disk));
    BOOST_CHECK(disk.nStatus & BLOCK_HAVE_UNDO);
    BOOST_CHECK_EQUAL(disk.nUndoPos, claimed->nPos + node::STORAGE_HEADER_BYTES);
    BOOST_CHECK(!(tip->nStatus & BLOCK_HAVE_UNDO));

    // Put back as it was.
    blockman.UndoWriteSettled(*tip);
    tip->nStatus = status;
    tip->nUndoPos = undo_pos;
    blockman.MarkDirty(tip);
}

//! In a reorganisation the transactions of the disconnected blocks are back in the mempool, and announced,
//! before the blocks of the new branch are announced connected. With `periodic_due` the periodic write-out
//! is due when the reorganisation starts. With `pinned_over_limit` the first block the reorganisation
//! connects takes the account of pinned blocks over its limit.
static void CheckReorgAnnouncementOrder(TestChain100Setup& setup, bool periodic_due = false, bool pinned_over_limit = false)
{
    auto& m_node{setup.m_node};
    auto& m_coinbase_txns{setup.m_coinbase_txns};
    auto& coinbaseKey{setup.coinbaseKey};
    const auto CreateAndProcessBlock{[&](const std::vector<CMutableTransaction>& txns, const CScript& script) { return setup.CreateAndProcessBlock(txns, script); }};
    const auto CreateValidMempoolTransaction{[&](CTransactionRef in, uint32_t vout, int height, CKey key, CScript out, CAmount amount) {
        return setup.CreateValidMempoolTransaction(in, vout, height, key, out, amount);
    }};
    struct Recorder final : CValidationInterface {
        std::vector<std::string> events;
        void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t) override { events.push_back("A " + tx.info.m_tx->GetHash().ToString()); }
        void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>& block, const CBlockIndex*) override { events.push_back("C " + block->GetHash().ToString()); }
    };
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{CScript() << OP_TRUE};

    // Two empty blocks on top of 100, set aside; then a block 101 that mines two mempool transactions, the
    // second spending the first.
    const CBlock fork1{CreateAndProcessBlock({}, script)};
    const CBlock fork2{CreateAndProcessBlock({}, script)};
    CBlockIndex* const fork1_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(fork1.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, fork1_index));
    const CMutableTransaction tx{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), 49 * COIN)};
    const CMutableTransaction tx2{CreateValidMempoolTransaction(MakeTransactionRef(tx), 0, 101, coinbaseKey, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), 48 * COIN)};
    const CBlock mined{CreateAndProcessBlock({tx, tx2}, script)};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), mined.GetHash());
    BOOST_REQUIRE_EQUAL(WITH_LOCK(m_node.mempool->cs, return m_node.mempool->size()), 0U);

    // The two blocks back: the reorganisation disconnects 101 and connects them.
    auto recorder{std::make_shared<Recorder>()};
    m_node.validation_signals->RegisterSharedValidationInterface(recorder);
    {
        LOCK(::cs_main);
        chainstate.ResetBlockFailureFlags(fork1_index);
        chainman.RecalculateBestHeader();
    }
    // Past the interval of the periodic write-out: the first write-out check that asks for it finds it due.
    if (periodic_due) setup.m_clock += std::chrono::hours{2};
    // A limit of one byte, which a connected block whose signal has not left is over. A step that is not of
    // a reorganisation sets the limit anew, so it is the reorganisation's step that meets this one.
    util::ByteAccount& pinned{chainman.m_bytes_in_flight[util::ByteAccountId::PinnedBlocks]};
    if (pinned_over_limit) pinned.SetLimit(1);
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    // Both blocks were connected by that step: no other step ran and set the limit.
    if (pinned_over_limit) BOOST_CHECK_EQUAL(pinned.Limit(), 1U);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(recorder);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), fork2.GetHash());

    const std::vector<std::string> expected{"A " + CTransaction{tx}.GetHash().ToString(), "A " + CTransaction{tx2}.GetHash().ToString(), "C " + fork1.GetHash().ToString(), "C " + fork2.GetHash().ToString()};
    BOOST_CHECK_EQUAL_COLLECTIONS(recorder->events.begin(), recorder->events.end(), expected.begin(), expected.end());
}

BOOST_FIXTURE_TEST_CASE(reorg_announces_readded_transactions_before_connected_blocks, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this);
}

//! The same with a write-out run started at every block, inside the reorganisation's step too: a run's settle
//! of the effects queue must not hand on the connected blocks' signals before the re-added transactions'.
struct WriteOutEveryBlockSetup : TestChain100Setup {
    WriteOutEveryBlockSetup() : TestChain100Setup{ChainType::REGTEST, {.extra_args = {"-test=write_out_every_block"}}} {}
};

//! A block whose write-out fails does not become the tip, and is not announced: the write is made before
//! the tip moves, as it always was.
BOOST_FIXTURE_TEST_CASE(failed_write_out_leaves_the_tip, WriteOutEveryBlockSetup)
{
    struct Recorder final : CValidationInterface {
        std::atomic<int> connected{0};
        std::atomic<int> tips{0};
        void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>&, const CBlockIndex*) override { ++connected; }
        void UpdatedBlockTip(const CBlockIndex*, const CBlockIndex*, bool) override { ++tips; }
    };
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BOOST_REQUIRE(chainman.m_options.test_write_out_every_block);
    const CBlockIndex* const old_tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};

    // The block is stored, and not connected yet.
    const auto block{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_TRUE))};
    BlockValidationState state;
    CBlockIndex* index{nullptr};
    bool new_block{false};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.AcceptBlock(block, state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true)));
    BOOST_REQUIRE(new_block && index);

    // Its block file cannot be opened from here on: the run its connection starts fails where it syncs the
    // file.
    const fs::path file{WITH_LOCK(::cs_main, return chainman.m_blockman.GetBlockPosFilename(index->GetBlockPos()))};
    BOOST_REQUIRE(fs::remove(file));
    BOOST_REQUIRE(fs::create_directory(file));

    const auto recorder{std::make_shared<Recorder>()};
    m_node.validation_signals->RegisterSharedValidationInterface(recorder);
    BOOST_CHECK(!chainstate.ActivateBestChain(state, block));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(recorder);

    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()) == old_tip);
    BOOST_CHECK_EQUAL(recorder->connected.load(), 0);
    BOOST_CHECK_EQUAL(recorder->tips.load(), 0);
}

//! Drives a step of the connect window one block at a time, with nothing added to the node: the connect
//! pool's workers are held by tasks that wait (as threadpool_tests' BlockWorkers() does), ActivateBestChain()
//! runs on a thread of its own, and the test thread runs the tasks the step queues, one per call
//! (ThreadPool::ProcessTask()). Between two calls the connector waits for a block no worker will apply, so
//! what the test does there happens between two blocks of the window. Not more than that: WaitForStep()
//! returns once the tip is published and the queue has its count, and where the window had nothing to
//! refill the connector may still be at what follows a publication (a periodic write-out, the wait for the
//! signal queue, the plan of the next blocks).
//!
//! What this reaches: blocks that complete in the order they were staged, which is the order of the queue.
//! A block that completes ahead of a lower one is out of its reach, and with it a deferred transaction
//! whose coin a lower block brings later. A block connected outside the window stalls under it, its input
//! fetch waits for tasks of the same pool: the workers are let go (Finish()) before a step comes to one.
//!
//! In a step of the window the queue holds the blocks' tasks only: a block read by its task is not freed
//! through the pool, and a write-out of a chain this small has one range, which its own thread writes.
class WindowStepper
{
    ChainstateManager& m_chainman;
    ThreadPool& m_pool;
    std::counting_semaphore<> m_release{0};
    std::vector<std::future<void>> m_held;
    bool m_released{false};
    std::thread m_connector;

    void ReleaseWorkers()
    {
        if (std::exchange(m_released, true)) return;
        m_release.release(static_cast<std::ptrdiff_t>(m_held.size()));
    }

public:
    explicit WindowStepper(ChainstateManager& chainman) : m_chainman{chainman}, m_pool{*chainman.m_connect_pool}
    {
        const size_t workers{m_pool.WorkersCount()};
        BOOST_REQUIRE_GT(workers, 0U);
        std::latch held{static_cast<std::ptrdiff_t>(workers)};
        for (size_t i{0}; i < workers; ++i) {
            m_held.push_back(*Assert(m_pool.Submit([&] {
                held.count_down();
                m_release.acquire();
            })));
        }
        held.wait();
    }
    //! Lets the workers go, which finish a step that a failed check left unfinished, and waits for it.
    ~WindowStepper()
    {
        ReleaseWorkers();
        for (auto& held : m_held) held.wait();
        if (m_connector.joinable()) m_connector.join();
    }

    //! Run `activate` (a call of ActivateBestChain()) beside the test.
    void Start(std::function<void()> activate) { m_connector = std::thread{std::move(activate)}; }
    //! Wait for it. The workers are let go first: they run what the step has queued and queues from here
    //! on, where a join alone would wait for ever.
    void Finish()
    {
        ReleaseWorkers();
        m_connector.join();
    }

    //! The tip's height, or nothing while another thread holds cs_main: the wait below is not to hang on a
    //! connector that waits under the lock for a block no worker runs.
    std::optional<int> TipHeight() const
    {
        TRY_LOCK(::cs_main, locked);
        if (!locked) return std::nullopt;
        return m_chainman.ActiveChain().Height();
    }

    //! Wait until the connector is at rest with `height` as the tip and `queued` blocks handed to the pool.
    void WaitForStep(int height, size_t queued)
    {
        const auto deadline{SteadyClock::now() + 60s};
        while (true) {
            const std::optional<int> tip{TipHeight()};
            const size_t in_queue{m_pool.WorkQueueSize()};
            if (tip == height && in_queue == queued) return;
            BOOST_REQUIRE_MESSAGE(SteadyClock::now() < deadline,
                                  strprintf("the step did not reach height %d with %u blocks queued: height %s, %u queued",
                                            height, queued, tip ? util::ToString(*tip) : "unknown (cs_main is held)", in_queue));
            UninterruptibleSleep(1ms);
        }
    }

    //! Run the task of the lowest block in flight, here.
    void RunLowestBlock() { BOOST_REQUIRE(m_pool.ProcessTask()); }
};

//! The signals of a window step, as a subscriber gets them.
struct WindowStepRecorder final : CValidationInterface {
    std::mutex mutex;
    std::vector<int> connected;
    std::vector<std::pair<int, bool>> tips;
    void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>&, const CBlockIndex* index) override
    {
        const std::lock_guard lock{mutex};
        connected.push_back(index->nHeight);
    }
    void UpdatedBlockTip(const CBlockIndex* index, const CBlockIndex*, bool initial_download) override
    {
        const std::lock_guard lock{mutex};
        tips.emplace_back(index->nHeight, initial_download);
    }
};

//! A chain of BLOCKS blocks above the fixture's, stored and never connected (their undo data is still to be
//! written), with the node in initial block download and its tip too old to leave it: what the next
//! ActivateBestChain() connects in the window.
struct WindowStepSetup : TestChain100Setup {
    static constexpr int BLOCKS{6};
    int m_base_height{0};
    std::shared_ptr<WindowStepRecorder> m_recorder{std::make_shared<WindowStepRecorder>()};

    WindowStepSetup()
    {
        ChainstateManager& chainman{*Assert(m_node.chainman)};
        Chainstate& chainstate{chainman.ActiveChainstate()};
        m_base_height = WITH_LOCK(::cs_main, return chainstate.m_chain.Height());
        // The blocks are made by a chain that is connected and then set aside for good. A copy of each with
        // another nonce, on top of the copy below it, is stored: a block that was never connected.
        std::vector<CBlock> blocks;
        for (int i{0}; i < BLOCKS; ++i) blocks.push_back(CreateAndProcessBlock({}, CScript() << OP_TRUE));
        CBlockIndex* const first{WITH_LOCK(::cs_main, return chainstate.m_chain[m_base_height + 1])};
        const int64_t tip_time{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockTime())};
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.InvalidateBlock(state, first));
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        // The blocks' signals held them in the pinned account, and their disconnection found it empty.
        CheckAccountsEmpty();
        {
            LOCK(::cs_main);
            uint256 prev{first->pprev->GetBlockHash()};
            for (CBlock& block : blocks) {
                block.hashPrevBlock = prev;
                ++block.nNonce;
                while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
                BlockValidationState accept_state;
                CBlockIndex* index{nullptr};
                bool new_block{false};
                BOOST_REQUIRE(chainman.AcceptBlock(std::make_shared<const CBlock>(block), accept_state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
                BOOST_REQUIRE(new_block && index && index->GetUndoPos().IsNull());
                prev = block.GetHash();
            }
        }
        m_clock.set(std::chrono::seconds{tip_time} + 48h);
        static_cast<TestChainstateManager&>(chainman).ResetIbd();
        BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);
        BOOST_REQUIRE_GT(chainman.ConnectWindow(), 1);
        m_node.validation_signals->RegisterSharedValidationInterface(m_recorder);
    }

    int Top() const { return m_base_height + BLOCKS; }

    //! The blocks in flight once the tip is at `height`.
    size_t InFlightAt(int height) const
    {
        return static_cast<size_t>(std::min(Top() - height, Assert(m_node.chainman)->ConnectWindow()));
    }

    //! Every byte charged for a block in flight has been released.
    void CheckAccountsEmpty() const
    {
        const ChainstateManager& chainman{*Assert(m_node.chainman)};
        for (const util::ByteAccountId id : util::BYTE_ACCOUNTS) {
            BOOST_CHECK_EQUAL(chainman.m_bytes_in_flight[id].Bytes(), 0U);
        }
    }

    //! The step is over: its signals are delivered, every block's BlockConnected and every block's
    //! UpdatedBlockTip in order, and the accounts of the bytes in flight are back at zero.
    void CheckStepOver()
    {
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);
        CheckAccountsEmpty();
        std::vector<int> expected;
        for (int height{m_base_height + 1}; height <= Top(); ++height) expected.push_back(height);
        const std::lock_guard lock{m_recorder->mutex};
        BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
        std::vector<int> tips;
        for (const auto& [height, initial_download] : m_recorder->tips) tips.push_back(height);
        BOOST_CHECK_EQUAL_COLLECTIONS(tips.begin(), tips.end(), expected.begin(), expected.end());
    }
};

//! The window's blocks are published one by one as their tasks are run, the window is refilled behind each,
//! and the accounts of the bytes in flight are back at zero when the step is over.
BOOST_FIXTURE_TEST_CASE(window_step_block_by_block, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BlockValidationState state;
    bool activated{false};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        BOOST_CHECK_GT(chainman.m_bytes_in_flight[util::ByteAccountId::Window].Bytes(), 0U);
        for (int height{m_base_height + 1}; height <= Top(); ++height) {
            stepper.RunLowestBlock();
            stepper.WaitForStep(height, InFlightAt(height));
            BOOST_CHECK(chainman.IsInitialBlockDownload());
        }
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    CheckStepOver();
    const std::lock_guard lock{m_recorder->mutex};
    for (const auto& [height, initial_download] : m_recorder->tips) BOOST_CHECK(initial_download);
}

//! The write-out that only a step of the window makes: the periodic one, started in the hold that published
//! a block and waited for once cs_main is released. (The write-out before the tip moves is the tip's, which
//! failed_write_out_leaves_the_tip covers.) When it fails the step ends where the run was waited for: the
//! block that started it is the tip and was announced (the periodic write follows the tip's notification),
//! no block above it is published, and the pass fails.
BOOST_FIXTURE_TEST_CASE(failed_periodic_write_out_ends_a_window_step, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};

    // The step's blocks are in the block cache, as blocks that were just received are: no task reads a file.
    BOOST_REQUIRE_GT(chainman.m_block_cache.Budget(), 0U);
    for (int height{m_base_height + 1}; height <= Top(); ++height) {
        const CBlockIndex* const index{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(height))};
        const auto block{std::make_shared<CBlock>()};
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(*block, *index));
        chainman.m_block_cache.Insert(height, block, m_base_height);
    }
    // A write sets the timer of the periodic one, and the clock goes past its interval: the periodic write
    // is due at the step's first block.
    {
        BlockValidationState flush_state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(flush_state, FlushStateMode::FORCE_FLUSH));
        m_clock += std::chrono::hours{2};
    }
    // A block beside the window's first is stored, which makes its block file owe a sync, and the file
    // cannot be opened from here on: the next run fails where it syncs it.
    {
        const auto beside{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_2))};
        LOCK(::cs_main);
        BlockValidationState accept_state;
        CBlockIndex* index{nullptr};
        bool new_block{false};
        BOOST_REQUIRE(chainman.AcceptBlock(beside, accept_state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
        BOOST_REQUIRE(new_block && index);
        const fs::path file{chainman.m_blockman.GetBlockPosFilename(index->GetBlockPos())};
        BOOST_REQUIRE(fs::remove(file));
        BOOST_REQUIRE(fs::create_directory(file));
    }

    BlockValidationState state;
    bool activated{true};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state, nullptr); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        // The first block is published: the write-out before the tip has nothing due, the periodic write
        // is, and the run its hold starts fails.
        stepper.RunLowestBlock();
        // The blocks above it are run by the workers, and taken back.
        stepper.Finish();
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);

    BOOST_CHECK(!activated);
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height + 1);
    // The blocks above gave their places in the store back.
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().OpenBlocks()), 0U);
    // And what they held in memory; the block cache alone keeps the blocks that were not connected.
    for (const util::ByteAccountId id : {util::ByteAccountId::Window, util::ByteAccountId::UndoBacklog, util::ByteAccountId::PinnedBlocks}) {
        BOOST_CHECK_EQUAL(chainman.m_bytes_in_flight[id].Bytes(), 0U);
    }
    const std::vector<int> expected{m_base_height + 1};
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
    std::vector<int> tips;
    for (const auto& [height, initial_download] : m_recorder->tips) tips.push_back(height);
    BOOST_CHECK_EQUAL_COLLECTIONS(tips.begin(), tips.end(), expected.begin(), expected.end());
}

//! A window block whose file cannot be read. Its task ends without a verdict, and the step ends where the
//! block would be published: the window is drained, the blocks give their places in the store back, the
//! node gets a fatal error and the pass fails with the tip where it was. Nothing is published or announced.
BOOST_FIXTURE_TEST_CASE(unreadable_block_ends_a_window_step, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};

    // The step's blocks are read from their file by their tasks (the fixture puts none in the block cache),
    // and the file is not there.
    const CBlockIndex* const first{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 1))};
    const fs::path file{WITH_LOCK(::cs_main, return chainman.m_blockman.GetBlockPosFilename(first->GetBlockPos()))};
    const fs::path aside{file + ".aside"};
    fs::rename(file, aside);

    BlockValidationState state;
    bool activated{true};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        // The first block's task finds no file. The step waits for the blocks above it before it ends,
        // which the workers run.
        stepper.RunLowestBlock();
        stepper.Finish();
    }
    fs::rename(aside, file);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);

    BOOST_CHECK(!activated);
    BOOST_CHECK(state.IsError());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "Failed to read block.");
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().OpenBlocks()), 0U);
    CheckAccountsEmpty();
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK(m_recorder->connected.empty());
    BOOST_CHECK(m_recorder->tips.empty());
}

//! A block of the window with a transaction whose coin no block creates. The transaction is deferred, since
//! the block's parent was not the tip when it was staged, and its continuation finds no coin, which in the
//! window decides nothing: the step ends, the changes of the block above it are taken back and then its own,
//! and the block is connected alone, where the same transaction makes it invalid. The block below it stays
//! the tip.
BOOST_FIXTURE_TEST_CASE(failing_deferral_ends_a_window_step, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    CBlockIndex* const second{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 2))};
    const CBlockIndex* const third{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 3))};
    // A sibling of the setup's second block with one transaction more, which spends no coin.
    CBlock bad;
    BOOST_REQUIRE(chainman.m_blockman.ReadBlock(bad, *second));
    CMutableTransaction spend;
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(m_rng.rand256()), 0});
    spend.vout.emplace_back(CAmount{1}, CScript() << OP_TRUE);
    bad.vtx.push_back(MakeTransactionRef(std::move(spend)));
    node::RegenerateCommitments(bad, chainman);
    while (!CheckProofOfWork(bad.GetHash(), bad.nBits, chainman.GetConsensus())) ++bad.nNonce;
    CBlockIndex* bad_index{nullptr};
    {
        LOCK(::cs_main);
        BlockValidationState accept_state;
        bool new_block{false};
        BOOST_REQUIRE(chainman.AcceptBlock(std::make_shared<const CBlock>(bad), accept_state, &bad_index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
        BOOST_REQUIRE(new_block && bad_index);
    }
    // And a block on top of it, the setup's third with the sibling as its parent: a block of the window is
    // in flight above the one that fails, and its changes are taken back first.
    CBlock above;
    BOOST_REQUIRE(chainman.m_blockman.ReadBlock(above, *third));
    above.hashPrevBlock = bad.GetHash();
    while (!CheckProofOfWork(above.GetHash(), above.nBits, chainman.GetConsensus())) ++above.nNonce;
    {
        LOCK(::cs_main);
        BlockValidationState accept_state;
        CBlockIndex* above_index{nullptr};
        bool new_block{false};
        BOOST_REQUIRE(chainman.AcceptBlock(std::make_shared<const CBlock>(above), accept_state, &above_index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
        BOOST_REQUIRE(new_block && above_index);
    }
    // The setup's blocks from the second on are set aside, so that the sibling is what the step heads for.
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(invalidate_state, second));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);

    BlockValidationState state;
    bool activated{false};
    {
        // Only a block that is connected alone logs this line (a window block is read by its task): the
        // block was connected a second time, outside the window. That its transaction was deferred the
        // first time, and not judged with the rest of the block, is not checked.
        ASSERT_DEBUG_LOG("  - Load block from disk");
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, std::min<size_t>(3, chainman.ConnectWindow()));
        stepper.RunLowestBlock();
        stepper.WaitForStep(m_base_height + 1, 2);
        // The sibling's task defers the transaction and runs its continuation, here, with the block above
        // it still queued. The workers are let go for that block, which the step waits for before it takes
        // the two back, and for the sibling connected alone next, outside the window.
        stepper.RunLowestBlock();
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    // The verdict is that of the step that connected the block alone: the window gives none.
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height + 1);
    BOOST_CHECK(WITH_LOCK(::cs_main, return (bad_index->nStatus & BLOCK_FAILED_VALID) != 0));
    // The block's place in the store was given back both times, and that of the block above it.
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().OpenBlocks()), 0U);
    BOOST_CHECK(!WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(COutPoint{bad.vtx.back()->GetHash(), 0})));
    BOOST_CHECK(!WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(COutPoint{above.vtx[0]->GetHash(), 0})));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);
    CheckAccountsEmpty();
    const std::vector<int> expected{m_base_height + 1};
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
}

//! The window's account is the node's, and only a step that uses the window writes it: a block connected
//! outside the window leaves its limit and its count as they are.
BOOST_FIXTURE_TEST_CASE(step_outside_the_window_leaves_its_account, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    util::ByteAccount& window{chainman.m_bytes_in_flight[util::ByteAccountId::Window]};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    const size_t limit{size_t{1} << 40};
    const size_t bytes{777};
    window.SetLimit(limit);
    window.Set(bytes);
    const int height{WITH_LOCK(::cs_main, return chainman.ActiveChain().Height())};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()), height + 1);
    BOOST_CHECK_EQUAL(window.Limit(), limit);
    BOOST_CHECK_EQUAL(window.Bytes(), bytes);
    window.Set(0);
}

//! The download ends while blocks handed to the window in it are still to be connected: the step goes on in
//! the window, the mode it was planned in, and the blocks it connects from there are announced as blocks
//! at the tip.
BOOST_FIXTURE_TEST_CASE(window_step_outlives_the_download, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BlockValidationState state;
    bool activated{false};
    const int last_in_download{m_base_height + 1};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        stepper.RunLowestBlock();
        stepper.WaitForStep(last_in_download, InFlightAt(last_in_download));
        BOOST_REQUIRE(chainman.IsInitialBlockDownload());
        // The blocks above are in flight, handed over in the download. It ends here.
        BOOST_REQUIRE_GT(InFlightAt(last_in_download), 1U);
        static_cast<TestChainstateManager&>(chainman).JumpOutOfIbd();
        for (int height{last_in_download + 1}; height <= Top(); ++height) {
            stepper.RunLowestBlock();
            // The window is refilled all the same: the step keeps the mode it was planned in.
            stepper.WaitForStep(height, InFlightAt(height));
        }
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(!chainman.IsInitialBlockDownload());
    CheckStepOver();
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_REQUIRE(!m_recorder->tips.empty());
    for (const auto& [height, initial_download] : m_recorder->tips) {
        BOOST_CHECK_EQUAL(initial_download, height <= last_in_download);
    }
}

BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_run_in_the_step, WriteOutEveryBlockSetup)
{
    BOOST_REQUIRE(m_node.chainman->m_options.test_write_out_every_block);
    CheckReorgAnnouncementOrder(*this);
}

//! The same with the periodic write-out due: mempool acceptance makes no write-out check between two of the
//! transactions a reorganisation adds back, which would settle the effects queue and hand on the connected
//! blocks' signals before the second transaction's.
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_periodic_write, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/true);
}
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_periodic_write_and_runs, WriteOutEveryBlockSetup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/true);
}

//! The same with the pinned blocks over their limit once the reorganisation has connected its first block.
//! The step does not wait there for the signals to leave, as a step that extends the tip does: the wait hands
//! on the signals that are ready, and the first block would be announced before the transactions.
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_pinned_blocks_over_their_limit, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/false, /*pinned_over_limit=*/true);
}

//! At the tip, the block that improves the tip and the pass's notifications are one hold of cs_main: a thread
//! that takes cs_main never sees a tip the kernel's tip notification has not named yet.
BOOST_FIXTURE_TEST_CASE(tip_and_its_notification_are_one_hold, TestChain100Setup)
{
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return m_node.chainman->IsInitialBlockDownload()));
    auto& notifications{*m_node.notifications};
    std::atomic<bool> stop{false};
    std::atomic<int> checks{0}, mismatches{0};
    std::thread observer{[&] {
        while (!stop) {
            LOCK(::cs_main);
            const uint256 tip{m_node.chainman->ActiveChain().Tip()->GetBlockHash()};
            const auto notified{WITH_LOCK(notifications.m_tip_block_mutex, return notifications.TipBlock())};
            if (notified && *notified != tip) ++mismatches;
            ++checks;
        }
    }};
    for (int i{0}; i < 200; ++i) CreateAndProcessBlock({}, CScript() << OP_TRUE);
    stop = true;
    observer.join();
    BOOST_CHECK(checks.load() > 0);
    BOOST_CHECK_EQUAL(mismatches.load(), 0);
}

//! A block handed to the connector (ProcessNewBlock() with activate_async) is the tip once
//! WaitForPendingBlocks() returns. A stopped connector takes nothing: the block is stored, nobody waits for
//! it, and whoever activates the chain next connects it.
BOOST_FIXTURE_TEST_CASE(connector_connects_what_it_is_handed, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    const CScript script{CScript() << OP_TRUE};
    const auto tip{[&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()); }};

    const auto first{std::make_shared<const CBlock>(CreateBlock({}, script))};
    BOOST_REQUIRE(chainman.ProcessNewBlock(first, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*activate_async=*/true));
    chainman.WaitForPendingBlocks(chainman.SyncModeNow());
    BOOST_CHECK_EQUAL(tip(), first->GetHash());

    chainman.StopConnector();
    const auto second{std::make_shared<const CBlock>(CreateBlock({}, script))};
    BOOST_REQUIRE(chainman.ProcessNewBlock(second, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*activate_async=*/true));
    chainman.WaitForPendingBlocks(chainman.SyncModeNow());
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(second->GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK(index->nStatus & BLOCK_HAVE_DATA);
    }
    BOOST_CHECK_EQUAL(tip(), first->GetHash());

    BlockValidationState state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    BOOST_CHECK_EQUAL(tip(), second->GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
