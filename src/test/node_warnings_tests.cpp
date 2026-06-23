// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//

#include <node/warnings.h>
#include <util/translation.h>

#include <test/util/setup_common.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(node_warnings_tests)

FIXTURE_TEST_CASE(warnings, BasicTestingSetup)
{
    node::Warnings warnings;
    // On pre-release builds, a warning is generated automatically
    warnings.Unset(node::Warning::PRE_RELEASE_TEST_BUILD);

    // For these tests, we don't care what the exact warnings are, so
    // just refer to them as warning_1 and warning_2
    const auto warning_1{node::Warning::CLOCK_OUT_OF_SYNC};
    const auto warning_2{node::Warning::FATAL_INTERNAL_ERROR};

    // Ensure we start without any warnings
    CHECK(warnings.GetMessages().size() == 0U);
    // Add two warnings
    CHECK(warnings.Set(warning_1, _("warning 1")));
    CHECK(warnings.Set(warning_2, _("warning 2")));
    // Unset the second one
    CHECK(warnings.Unset(warning_2));
    // Since it's already been unset, this should return false
    CHECK(!warnings.Unset(warning_2));
    // We should now be able to set w2 again
    CHECK(warnings.Set(warning_2, _("warning 2 - revision 1")));
    // Setting w2 again should return false since it's already set
    CHECK(!warnings.Set(warning_2, _("warning 2 - revision 2")));

    // Verify messages are correct
    const auto messages{warnings.GetMessages()};
    CHECK(messages.size() == 2U);
    CHECK(messages[0].original == "warning 1");
    CHECK(messages[1].original == "warning 2 - revision 1");

    // Clearing all warnings should also clear all messages
    CHECK(warnings.Unset(warning_1));
    CHECK(warnings.Unset(warning_2));
    CHECK(warnings.GetMessages().size() == 0U);
}

TEST_SUITE_END()
