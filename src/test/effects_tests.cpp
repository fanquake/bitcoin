// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/effects.h>
#include <sync.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using kernel::EffectsExecutor;

BOOST_AUTO_TEST_SUITE(effects_tests)

BOOST_AUTO_TEST_CASE(effects_run_and_complete_in_order)
{
    std::mutex m;
    std::vector<int> ran;
    std::vector<int> completed;
    EffectsExecutor exec;
    exec.Start();
    for (int i = 0; i < 64; ++i) {
        exec.Enqueue(
            [&, i] {
                // Vary the I/O time so a reordering executor would show it.
                if (i % 7 == 0) std::this_thread::sleep_for(std::chrono::milliseconds{2});
                std::lock_guard<std::mutex> l{m};
                ran.push_back(i);
                return i % 5 != 0; // some effects fail
            },
            [&, i](bool ok) {
                BOOST_CHECK_EQUAL(ok, i % 5 != 0);
                completed.push_back(i);
            },
            /*bytes=*/1);
    }
    exec.WaitForIO();
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 0U);
    {
        std::lock_guard<std::mutex> l{m};
        BOOST_REQUIRE_EQUAL(ran.size(), 64U);
        for (int i = 0; i < 64; ++i) BOOST_CHECK_EQUAL(ran[i], i);
    }
    // Completions are applied by the caller, in order, and only once.
    BOOST_CHECK(exec.HasPendingCompletions() || completed.size() == 64U);
    exec.ApplyCompletions();
    BOOST_REQUIRE_EQUAL(completed.size(), 64U);
    for (int i = 0; i < 64; ++i) BOOST_CHECK_EQUAL(completed[i], i);
    exec.ApplyCompletions();
    BOOST_CHECK_EQUAL(completed.size(), 64U);
    exec.Stop();
}

BOOST_AUTO_TEST_CASE(effects_backlog_and_stop_drain)
{
    std::atomic<int> ran{0};
    std::atomic<bool> release{false};
    EffectsExecutor exec;
    exec.Start();
    // The first effect blocks until released, holding everything behind it.
    exec.Enqueue([&] { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds{1}); ++ran; return true; }, {}, 10);
    for (int i = 0; i < 9; ++i) exec.Enqueue([&] { ++ran; return true; }, {}, 10);
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 100U);

    std::thread waiter{[&] { exec.WaitForBacklogBelow(50); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    BOOST_CHECK_EQUAL(ran.load(), 0);
    release = true;
    waiter.join();
    BOOST_CHECK_LE(exec.PendingBytes(), 50U);

    // Stop runs every remaining effect before returning.
    exec.Stop();
    BOOST_CHECK_EQUAL(ran.load(), 10);
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 0U);
    exec.Stop(); // idempotent
}

BOOST_AUTO_TEST_CASE(effects_completion_hook_runs_after_each_effect)
{
    std::atomic<int> hooked{0};
    std::vector<int> completed;
    EffectsExecutor exec{[&] { ++hooked; }};
    exec.Start();
    for (int i = 0; i < 5; ++i) exec.Enqueue([] { return true; }, [&, i](bool) { completed.push_back(i); });
    exec.WaitForIO();
    // The hook ran (at least once per effect; it may retry while completions are pending) but the
    // completions themselves are applied only by ApplyCompletions().
    BOOST_CHECK_GE(hooked.load(), 5);
    BOOST_CHECK(completed.empty());
    exec.ApplyCompletions();
    BOOST_CHECK_EQUAL(completed.size(), 5U);
}

BOOST_AUTO_TEST_SUITE_END()
