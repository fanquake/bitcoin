// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/effects.h>
#include <test/util/setup_common.h>
#include <util/byte_ledger.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using util::ByteAccount;
using util::ByteLedger;
using Id = util::ByteAccountId;

BOOST_FIXTURE_TEST_SUITE(byte_ledger_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(an_account_admits_up_to_its_limit)
{
    ByteAccount account;
    account.SetLimit(100);
    BOOST_CHECK(account.Admits());
    BOOST_CHECK(account.Admits(100));
    BOOST_CHECK(!account.Admits(101));

    account.Add(60);
    BOOST_CHECK(account.Admits(40));
    BOOST_CHECK(!account.Admits(41));

    // A holder may go over (one block is always in flight, whatever it weighs): nothing more is admitted
    // until it is back within the limit.
    account.Add(60);
    BOOST_CHECK_EQUAL(account.Bytes(), 120U);
    BOOST_CHECK(!account.Admits());
    account.Sub(20);
    BOOST_CHECK(account.Admits());
    BOOST_CHECK(!account.Admits(1));
}

BOOST_AUTO_TEST_CASE(an_account_without_a_limit_is_counted_and_admits_everything)
{
    ByteAccount account;
    BOOST_CHECK_EQUAL(account.Limit(), 0U);
    account.Add(size_t{1} << 40);
    BOOST_CHECK(account.Admits(size_t{1} << 40));
    BOOST_CHECK_EQUAL(account.Bytes(), size_t{1} << 40);
}

BOOST_AUTO_TEST_CASE(the_executor_wakes_a_requester_for_its_completions)
{
    ByteLedger ledger;
    ByteAccount& pinned{ledger[Id::PinnedBlocks]};
    pinned.SetLimit(20);
    std::atomic<bool> release{false};
    // No hook: the requester applies the completions, as the connector does where the hook cannot.
    ByteAccount backlog;
    kernel::EffectsExecutor exec{backlog};
    exec.Start();
    exec.Enqueue([&] { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds{1}); return true; }, {});
    // Five signals behind the blocked effect, each holding 10 bytes until its completion is applied.
    for (int i = 0; i < 5; ++i) {
        pinned.Add(10);
        exec.Enqueue(nullptr, [&](bool) { pinned.Sub(10); });
    }
    BOOST_CHECK(!pinned.Admits());

    std::thread requester{[&] {
        while (!pinned.Admits()) {
            exec.WaitForCompletionOr([&] { return pinned.Admits(); });
            exec.ApplyCompletions();
        }
    }};
    release = true;
    requester.join();
    BOOST_CHECK(pinned.Admits());

    // With nothing queued or running and no completion left, the wait returns at once.
    exec.WaitForIO();
    exec.ApplyCompletions();
    BOOST_CHECK_EQUAL(pinned.Bytes(), 0U);
    exec.WaitForCompletionOr([] { return false; });
    // And where the caller's condition holds already, whatever is queued.
    std::atomic<bool> release_again{false};
    exec.Enqueue([&] { while (!release_again) std::this_thread::sleep_for(std::chrono::milliseconds{1}); return true; }, {});
    exec.WaitForCompletionOr([] { return true; });
    release_again = true;

    exec.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
