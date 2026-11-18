// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/sync_mode.h>

#include <sync.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <future>

using namespace std::chrono_literals;

using node::SyncMode;

BOOST_AUTO_TEST_SUITE(sync_mode_tests)

BOOST_AUTO_TEST_CASE(mode_of_the_latch)
{
    BOOST_CHECK(node::SyncModeOf(true) == SyncMode::Download);
    BOOST_CHECK(node::SyncModeOf(false) == SyncMode::Tip);
}

//! The waits and the yield go by the copy they are given, not by the latch: an operation that started in
//! the download finishes as one of the download after the latch fell.
BOOST_FIXTURE_TEST_CASE(a_copy_outlives_the_latch, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    BOOST_CHECK(chainman.SyncModeNow() == SyncMode::Tip);
    // Nothing is queued: neither mode gives way, and neither wait has anything to wait for.
    BOOST_CHECK(!chainman.YieldHistoricalActivation(SyncMode::Download));
    BOOST_CHECK(!chainman.YieldHistoricalActivation(SyncMode::Tip));
    chainman.WaitForPendingBlocks(SyncMode::Download);
    chainman.WaitForPendingBlocks(SyncMode::Tip);

    // An activation is handed to the connector, which cannot attempt it while cs_main is held here.
    std::future<void> download;
    std::future<void> tip;
    {
        LOCK(::cs_main);
        std::async(std::launch::async, [&] { chainman.SignalConnector(); }).get();
        download = std::async(std::launch::async, [&] { chainman.WaitForPendingBlocks(SyncMode::Download); });
        tip = std::async(std::launch::async, [&] { chainman.WaitForPendingBlocks(SyncMode::Tip); });
        // The latch says tip. The wait that was given the download's copy goes by the copy: it returns with
        // the activation still to be attempted.
        BOOST_REQUIRE(download.wait_for(60s) == std::future_status::ready);
        BOOST_CHECK(chainman.AnyHandOffPending());
        // The wait that was given the tip's copy is still waiting. (A slow thread can make this pass, not fail.)
        BOOST_CHECK(tip.wait_for(100ms) == std::future_status::timeout);
    }
    // With cs_main free the connector attempts the activation, and that wait returns too.
    tip.get();
}

BOOST_AUTO_TEST_SUITE_END()
