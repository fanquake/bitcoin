// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <node/blockstore_machine.h>
#include <node/durability_machine.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/fs.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <memory>

using node::DurabilityState;
using node::FLUSH_TABLE;
using node::FlushPhase;
using node::FlushStep;

BOOST_AUTO_TEST_SUITE(durability_machine_tests)

BOOST_AUTO_TEST_CASE(flush_table)
{
    using S = FlushPhase;
    using E = FlushStep;
    // The order the write-out relies on, by name.
    BOOST_CHECK(!FLUSH_TABLE.Next(S::Start, E::WriteIndex)); // no index write before the files are synced
    BOOST_CHECK(!FLUSH_TABLE.Next(S::FilesSynced, E::Unlink)); // no unlink while the index still refers to the file
    BOOST_CHECK(!FLUSH_TABLE.Next(S::FilesSynced, E::WriteCoins)); // no coins ahead of the index
    BOOST_CHECK(!FLUSH_TABLE.Next(S::CoinsWritten, E::WriteCoins));
}

// A block's bytes are Unsynced from the write until the next write-out, which syncs every file
// written to before it records the block in the index; pruning the file leaves nothing to record.
BOOST_FIXTURE_TEST_CASE(durability_follows_the_files, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};

    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    BOOST_REQUIRE(tip);
    {
        LOCK(cs_main);
        BOOST_CHECK(blockman.DataDurability(*tip) == DurabilityState::Unsynced);
        BOOST_CHECK(blockman.UndoDurability(*tip) == DurabilityState::Unsynced);
    }

    // The write-out syncs the files, then writes the index.
    chainman.ActiveChainstate().ForceFlushStateToDisk();
    {
        LOCK(cs_main);
        BOOST_CHECK(blockman.DataDurability(*tip) == DurabilityState::Synced);
        BOOST_CHECK(blockman.UndoDurability(*tip) == DurabilityState::Synced);
    }

    // Sync on its own, after another write into the same files.
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
    {
        LOCK(cs_main);
        BOOST_CHECK(blockman.DataDurability(*tip) == DurabilityState::Unsynced);
        BOOST_CHECK(blockman.UndoDurability(*tip) == DurabilityState::Unsynced);
        BOOST_CHECK(blockman.SyncWrittenFiles());
        BOOST_CHECK(blockman.DataDurability(*tip) == DurabilityState::Synced);
        BOOST_CHECK(blockman.UndoDurability(*tip) == DurabilityState::Synced);

        blockman.m_have_pruned = true;
        blockman.PruneOneBlockFile(tip->nFile);
        BOOST_CHECK(blockman.DataDurability(*tip) == DurabilityState::Absent);
        BOOST_CHECK(blockman.UndoDurability(*tip) == DurabilityState::Absent);
    }
}

namespace {
//! The fault chainstate_flush_failure_boundary injects: with a directory in its place, the
//! next flush of the file fails where it opens it.
void InjectOpenFailure(const fs::path& file)
{
    Assert(fs::remove(file));
    Assert(fs::create_directory(file));
}

//! Take the fault away again: an empty file, which every flush succeeds on.
void RemoveOpenFailure(const fs::path& file)
{
    Assert(fs::remove(file));
    Assert(std::ofstream{static_cast<const std::filesystem::path&>(file)}.is_open());
}
} // namespace

// A failed flush is remembered. The write-out fails for the rest of the run, also once the
// file can be flushed again, so the index on disk never records the block.
BOOST_FIXTURE_TEST_CASE(failed_flush_fails_every_later_write_out, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    auto& blockman{chainman.m_blockman};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH));
    const CBlockIndex* flushed{WITH_LOCK(cs_main, return chainstate.GetLastFlushedBlock())};

    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    BOOST_REQUIRE(tip != flushed);
    const fs::path file{WITH_LOCK(cs_main, return blockman.GetBlockPosFilename(tip->GetBlockPos()))};

    InjectOpenFailure(file);
    BOOST_CHECK(!chainstate.FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH));
    BOOST_CHECK(state.IsError());

    RemoveOpenFailure(file);
    BlockValidationState later;
    BOOST_CHECK(!chainstate.FlushStateToDisk(later, FlushStateMode::FORCE_FLUSH));
    BOOST_CHECK(later.IsError());

    LOCK(cs_main);
    BOOST_CHECK(!blockman.SyncWrittenFiles());
    BOOST_CHECK(chainstate.GetLastFlushedBlock() == flushed);
    kernel::CBlockFileInfo file_info;
    BOOST_REQUIRE(blockman.m_block_tree_db->ReadBlockFileInfo(tip->nFile, file_info));
    BOOST_CHECK_EQUAL(file_info.nHeightLast, static_cast<unsigned int>(flushed->nHeight));
}

// SyncWrittenFiles() remembers a failed flush of its own, of a block file...
BOOST_FIXTURE_TEST_CASE(failed_block_file_sync_is_remembered, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    const fs::path file{WITH_LOCK(cs_main, return blockman.GetBlockPosFilename(tip->GetBlockPos()))};

    InjectOpenFailure(file);
    {
        LOCK(cs_main);
        BOOST_CHECK(!blockman.SyncWrittenFiles());
        BOOST_CHECK(blockman.DataDurability(*tip) == DurabilityState::Unsynced);
    }
    RemoveOpenFailure(file);
    BOOST_CHECK(!WITH_LOCK(cs_main, return blockman.SyncWrittenFiles()));
}

// ...and of an undo file.
BOOST_FIXTURE_TEST_CASE(failed_undo_file_sync_is_remembered, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    const fs::path block_file{WITH_LOCK(cs_main, return blockman.GetBlockPosFilename(tip->GetBlockPos()))};
    const int file_number{WITH_LOCK(cs_main, return tip->nFile)};
    const fs::path file{fs::path{block_file.parent_path()} / fs::PathFromString(strprintf("rev%05u.dat", file_number))};
    BOOST_REQUIRE(fs::exists(file));

    InjectOpenFailure(file);
    {
        LOCK(cs_main);
        BOOST_CHECK(!blockman.SyncWrittenFiles());
        BOOST_CHECK(blockman.DataDurability(*tip) == DurabilityState::Synced);
        BOOST_CHECK(blockman.UndoDurability(*tip) == DurabilityState::Unsynced);
    }
    RemoveOpenFailure(file);
    BOOST_CHECK(!WITH_LOCK(cs_main, return blockman.SyncWrittenFiles()));
}

namespace {
struct FastPruneChain100Setup : public TestChain100Setup {
    FastPruneChain100Setup() : TestChain100Setup{ChainType::REGTEST, {.extra_args = {"-fastprune"}}} {}
};

//! A block on top of `prev` that pays `value` to a script of `script_size` bytes.
std::shared_ptr<const CBlock> ChildBlock(const CBlockHeader& prev, int height, CAmount value, size_t script_size, const Consensus::Params& params)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << height << OP_0;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = value;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout[0].scriptPubKey.resize(script_size);

    CBlock block;
    block.nVersion = prev.nVersion;
    block.hashPrevBlock = prev.GetHash();
    block.nTime = prev.nTime + 1;
    block.nBits = prev.nBits;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, params)) ++block.nNonce;
    return std::make_shared<const CBlock>(std::move(block));
}
} // namespace

// The case the cursor file's sync does not cover: undo data written into the undo file of
// a block file the cursor has left. Only SyncWrittenFiles() makes it durable before the
// index records it; without that call the write-out's check in WriteBlockIndexDB fails.
BOOST_FIXTURE_TEST_CASE(undo_in_a_left_file_is_synced, FastPruneChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    const auto& params{chainman.GetConsensus()};
    const CBlockHeader tip_header{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHeader())};
    const int height{WITH_LOCK(cs_main, return chainman.ActiveChain().Height())};

    // Two small blocks into the current file, stored without being connected, and a block
    // too large for the rest of that file, so that the cursor moves on. The second block
    // pays itself too much: it will fail to connect, and the large one with it.
    const auto first{ChildBlock(tip_header, height + 1, 50 * COIN, 1, params)};
    const auto second{ChildBlock(*first, height + 2, 51 * COIN, 1, params)};
    const auto large{ChildBlock(*second, height + 3, 50 * COIN, 70'000, params)};
    CBlockIndex* first_index{nullptr};
    CBlockIndex* second_index{nullptr};
    CBlockIndex* large_index{nullptr};
    {
        LOCK(cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(chainman.AcceptBlock(first, state, &first_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(chainman.AcceptBlock(second, state, &second_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(chainman.AcceptBlock(large, state, &large_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(first_index && large_index);
        BOOST_REQUIRE_LT(first_index->nFile, large_index->nFile);
    }

    // Connect the first block: its undo data goes into the undo file that belongs to its
    // block file, which is neither the cursor's nor at the left file's last height.
    BlockValidationState state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainman.ActiveChain().Tip() == first_index);
        BOOST_CHECK(blockman.UndoDurability(*first_index) == DurabilityState::Unsynced);
    }

    chainman.ActiveChainstate().ForceFlushStateToDisk();
    {
        LOCK(cs_main);
        BOOST_CHECK(blockman.UndoDurability(*first_index) == DurabilityState::Synced);
    }

    // The block storage machine on the same blocks: the block that failed to connect and
    // its descendant are failed with their data, and pruning takes the data away.
    {
        LOCK(cs_main);
        const CChain& chain{chainman.ActiveChain()};
        using node::BlockStoreState;
        BOOST_CHECK(node::BlockStoreStateOf(*first_index, chain) == BlockStoreState::Connected);
        BOOST_CHECK(node::BlockStoreStateOf(*second_index, chain) == BlockStoreState::FailedStored);
        BOOST_CHECK(node::BlockStoreStateOf(*large_index, chain) == BlockStoreState::FailedStored);
        blockman.m_have_pruned = true;
        blockman.PruneOneBlockFile(large_index->nFile);
        BOOST_CHECK(node::BlockStoreStateOf(*large_index, chain) == BlockStoreState::FailedHeader);
    }
}

BOOST_AUTO_TEST_SUITE_END()
