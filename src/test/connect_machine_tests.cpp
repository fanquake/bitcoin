// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/connect_machine.h>

#include <boost/test/unit_test.hpp>

using node::WINDOW_BLOCK_TABLE;
using S = node::WindowBlockState;
using E = node::WindowBlockEvent;

BOOST_AUTO_TEST_SUITE(connect_machine_tests)

BOOST_AUTO_TEST_CASE(window_block_readings)
{
    // A block is published from a state nothing leads out of, so its outcome cannot change under the
    // publisher; and a continuation is claimed once: no event leads into Continuing but Claim.
    for (const S state : {S::Running, S::Applied, S::Deferred, S::Continuing, S::Finished}) {
        bool leaves{false};
        for (const E event : {E::Apply, E::ApplyDeferred, E::Claim, E::Finish}) {
            const auto next{WINDOW_BLOCK_TABLE.Next(state, event)};
            if (!next) continue;
            leaves = true;
            BOOST_CHECK(node::IsApplied(*next));
            BOOST_CHECK_EQUAL(node::IsContinuing(*next), event == E::Claim);
        }
        BOOST_CHECK_EQUAL(node::IsComplete(state), !leaves);
    }
    BOOST_CHECK(!node::IsApplied(S::Running));
    // The lower blocks wait for a deferred block no longer, the publisher still does.
    BOOST_CHECK(node::IsApplied(S::Deferred) && !node::IsComplete(S::Deferred));
    BOOST_CHECK(node::IsApplied(S::Continuing) && !node::IsComplete(S::Continuing));
}

BOOST_AUTO_TEST_SUITE_END()
