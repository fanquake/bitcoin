// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/check.h>

#include <test/util/framework.h>

#include <test/util/common.h>

TEST_SUITE_BEGIN(util_check_tests)

TEST_CASE(check_pass)
{
    Assume(true);
    Assert(true);
    CHECK_NONFATAL(true);
}

TEST_CASE(check_fail)
{
    // Disable aborts for easier testing here
    test_only_CheckFailuresAreExceptionsNotAborts mock_checks{};

    if constexpr (G_ABORT_ON_FAILED_ASSUME) {
        CHECK_EXCEPTION(Assume(false), NonFatalCheckError, HasReason{"Internal bug detected: false"});
    } else {
        CHECK_NOTHROW(Assume(false));
    }
    CHECK_EXCEPTION(Assert(false), NonFatalCheckError, HasReason{"Internal bug detected: false"});
    CHECK_EXCEPTION(CHECK_NONFATAL(false), NonFatalCheckError, HasReason{"Internal bug detected: false"});
}

TEST_SUITE_END()
