// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/handoff_machine.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using node::ConnectorHandOffs;
using node::HandOffTicket;

BOOST_FIXTURE_TEST_SUITE(handoff_machine_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(one_pass)
{
    ConnectorHandOffs handoffs;
    BOOST_CHECK(!handoffs.AnyQueued() && !handoffs.AnyUnattempted());
    const HandOffTicket first{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyQueued());
    handoffs.Take();
    BOOST_CHECK(!handoffs.AnyQueued() && handoffs.AnyUnattempted());
    BOOST_CHECK(!handoffs.ActiveChainAttempted(first));
    // A block handed over during the pass waits for the next one.
    const HandOffTicket second{handoffs.Hand(/*for_active=*/true)};
    handoffs.ActiveAttempted();
    BOOST_CHECK(handoffs.ActiveChainAttempted(first) && !handoffs.Attempted(first));
    BOOST_CHECK(handoffs.AnyQueued());
    BOOST_CHECK(!handoffs.ActiveChainAttempted(second));
    handoffs.AllAttempted();
    BOOST_CHECK(handoffs.Attempted(first) && !handoffs.Attempted(second));
    BOOST_CHECK(handoffs.AnyQueued());
}

BOOST_AUTO_TEST_CASE(a_pass_that_gave_way_is_done_with_the_next)
{
    // The historical chainstate's activation gave way to a block handed over during the pass: the connector
    // takes again without AllAttempted.
    ConnectorHandOffs handoffs;
    const HandOffTicket first{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyActiveQueued());
    handoffs.Take();
    BOOST_CHECK(!handoffs.AnyActiveQueued());
    handoffs.ActiveAttempted();
    // A block for the historical chainstate is nothing to give way to.
    const HandOffTicket historical{handoffs.Hand(/*for_active=*/false)};
    BOOST_CHECK(handoffs.AnyQueued() && !handoffs.AnyActiveQueued());
    const HandOffTicket second{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyActiveQueued());
    handoffs.Take();
    BOOST_CHECK(handoffs.ActiveChainAttempted(first) && !handoffs.Attempted(first));
    BOOST_CHECK(!handoffs.AnyQueued());
    BOOST_CHECK(!handoffs.ActiveChainAttempted(second) && !handoffs.Attempted(historical));
    handoffs.ActiveAttempted();
    BOOST_CHECK(handoffs.ActiveChainAttempted(first) && !handoffs.Attempted(first));
    BOOST_CHECK(handoffs.ActiveChainAttempted(second) && !handoffs.Attempted(second));
    handoffs.AllAttempted();
    BOOST_CHECK(handoffs.Attempted(first) && handoffs.Attempted(second));
    BOOST_CHECK(handoffs.Attempted(historical));
}

BOOST_AUTO_TEST_CASE(a_block_of_the_historical_chain_is_not_waited_for_by_the_active_one)
{
    ConnectorHandOffs handoffs;
    const HandOffTicket active{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    handoffs.ActiveAttempted();
    handoffs.AllAttempted();
    handoffs.Hand(/*for_active=*/false);
    BOOST_CHECK(handoffs.AnyQueued() && !handoffs.AnyActiveQueued());
    // A transaction that follows it waits for nothing; a ping does.
    BOOST_CHECK(handoffs.ActiveChainAttempted(handoffs.Latest()));
    BOOST_CHECK(!handoffs.Attempted(handoffs.Latest()));
    BOOST_CHECK(handoffs.Attempted(active));
}

BOOST_AUTO_TEST_CASE(stopped_stays_stopped)
{
    ConnectorHandOffs handoffs;
    const HandOffTicket done{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    handoffs.ActiveAttempted();
    handoffs.AllAttempted();
    const HandOffTicket taken{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    const HandOffTicket queued{handoffs.Hand(/*for_active=*/true)};
    handoffs.Stop();
    // The pass that was running when the connector was stopped ends, and changes nothing any more.
    handoffs.ActiveAttempted();
    handoffs.AllAttempted();
    handoffs.Take();
    const HandOffTicket late{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.Stopped());
    BOOST_CHECK(handoffs.Attempted(done));
    BOOST_CHECK(!handoffs.Attempted(taken) && !handoffs.ActiveChainAttempted(taken));
    BOOST_CHECK(!handoffs.Attempted(queued) && !handoffs.ActiveChainAttempted(queued));
    BOOST_CHECK(!handoffs.Attempted(late) && !handoffs.ActiveChainAttempted(late));
}

BOOST_AUTO_TEST_SUITE_END()
