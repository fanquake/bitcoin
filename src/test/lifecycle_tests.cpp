// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/lifecycle.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <thread>

using util::HookTarget;
using util::Lifecycle;
using S = util::LifeState;

BOOST_FIXTURE_TEST_SUITE(lifecycle_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(work_is_accepted_only_while_running)
{
    Lifecycle life;
    BOOST_CHECK(life.Stopped() && !life.Running());
    BOOST_CHECK(!life.DrainIfRunning());
    life.Start();
    BOOST_CHECK(life.Running() && !life.Stopped());
    BOOST_CHECK(life.DrainIfRunning());
    BOOST_CHECK(life.State() == S::Draining);
    BOOST_CHECK(!life.Running() && !life.Stopped());
    // A second stop does not begin another.
    BOOST_CHECK(!life.DrainIfRunning());
    life.Finish();
    BOOST_CHECK(life.Stopped());
    // An object may be started again.
    life.Start();
    BOOST_CHECK(life.Running());

    Lifecycle from_construction{S::Running};
    BOOST_CHECK(from_construction.Running());
}

BOOST_AUTO_TEST_CASE(a_disarmed_hook_does_not_reach_its_owner)
{
    int owner{0};
    const auto target{std::make_shared<HookTarget<int>>(owner)};
    const auto hook{[target] { return target->Run([](int& value) { ++value; }); }};
    BOOST_CHECK(hook());
    BOOST_CHECK_EQUAL(owner, 1);
    target->Disarm();
    BOOST_CHECK(!hook());
    BOOST_CHECK_EQUAL(owner, 1);
    target->Disarm(); // idempotent
    BOOST_CHECK(!hook());
}

BOOST_AUTO_TEST_CASE(disarming_waits_for_a_running_hook)
{
    struct Owner {
        std::atomic<bool> hook_left{false};
    };
    auto owner{std::make_unique<Owner>()};
    const auto target{std::make_shared<HookTarget<Owner>>(*owner)};
    std::promise<void> entered;
    std::promise<void> release;
    std::thread caller{[&, target] {
        const bool ran{target->Run([&](Owner& reached) {
            entered.set_value();
            release.get_future().wait();
            reached.hook_left = true;
        })};
        Assert(ran);
    }};
    entered.get_future().wait();
    std::thread stopper{[&] {
        target->Disarm();
        // The hook left the owner before Disarm() returned, whichever of the two threads ran first from
        // here: the owner may be destroyed now.
        Assert(owner->hook_left);
        owner.reset();
    }};
    release.set_value();
    caller.join();
    stopper.join();
    BOOST_CHECK(!owner);
    BOOST_CHECK(!target->Run([](Owner&) { Assert(false); }));
}

BOOST_AUTO_TEST_SUITE_END()
