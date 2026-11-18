// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/worker_task.h>

#include <boost/test/unit_test.hpp>

#include <ios>
#include <stdexcept>

BOOST_AUTO_TEST_SUITE(util_worker_task_tests)

static constexpr util::WorkerTaskType READ_TASK{"read block"};
static constexpr util::WorkerTaskType CHECK_TASK{"check block"};

BOOST_AUTO_TEST_CASE(success_is_no_failure)
{
    bool ran{false};
    BOOST_CHECK(!util::RunWorkerTask(READ_TASK, [&] { ran = true; }));
    BOOST_CHECK(ran);
}

BOOST_AUTO_TEST_CASE(runtime_error_becomes_a_value)
{
    const auto stops{util::RunWorkerTask(READ_TASK, [] { throw std::runtime_error{"short read"}; })};
    BOOST_REQUIRE(stops);
    BOOST_CHECK_EQUAL(stops->what, "read block: short read");

    // A class derived from std::runtime_error is handled the same way.
    const auto fails{util::RunWorkerTask(CHECK_TASK, [] { throw std::ios_base::failure{"eof"}; })};
    BOOST_REQUIRE(fails);
    BOOST_CHECK(fails->what.starts_with("check block: "));
}

BOOST_AUTO_TEST_CASE(nothing_wider_is_caught)
{
    // A logic error is a bug; it passes through (and would terminate a worker thread).
    BOOST_CHECK_THROW((void)util::RunWorkerTask(CHECK_TASK, [] { throw std::logic_error{"bug"}; }), std::logic_error);
}

BOOST_AUTO_TEST_SUITE_END()
