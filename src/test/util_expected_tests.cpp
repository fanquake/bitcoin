// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <test/util/setup_common.h>
#include <util/expected.h>

#include <test/util/framework.h>

#include <memory>
#include <string>
#include <utility>


using namespace util;

TEST_SUITE_BEGIN(util_expected_tests)

TEST_CASE(expected_value)
{
    struct Obj {
        int x;
    };
    Expected<Obj, int> e{};
    CHECK(e.value().x == 0);

    e = Obj{42};

    CHECK(e.has_value());
    CHECK(static_cast<bool>(e));
    CHECK(e.value().x == 42);
    CHECK((*e).x == 42);
    CHECK(e->x == 42);

    // modify value
    e.value().x += 1;
    (*e).x += 1;
    e->x += 1;

    const auto& read{e};
    CHECK(read.value().x == 45);
    CHECK((*read).x == 45);
    CHECK(read->x == 45);
}

TEST_CASE(expected_value_rvalue)
{
    Expected<std::unique_ptr<int>, int> no_copy{std::make_unique<int>(5)};
    const auto moved{std::move(no_copy).value()};
    CHECK(*moved == 5);
}

TEST_CASE(expected_deref_rvalue)
{
    Expected<std::unique_ptr<int>, int> no_copy{std::make_unique<int>(5)};
    const auto moved{*std::move(no_copy)};
    CHECK(*moved == 5);
}

TEST_CASE(expected_value_or)
{
    Expected<std::unique_ptr<int>, int> no_copy{std::make_unique<int>(1)};
    const int one{*std::move(no_copy).value_or(std::make_unique<int>(2))};
    CHECK(one == 1);

    const Expected<std::string, int> const_val{Unexpected{-1}};
    CHECK(const_val.value_or("fallback") == "fallback");
}

TEST_CASE(expected_value_throws)
{
    const Expected<int, std::string> e{Unexpected{"fail"}};
    CHECK_THROWS_AS(e.value(), BadExpectedAccess);

    const Expected<void, std::string> void_e{Unexpected{"fail"}};
    CHECK_THROWS_AS(void_e.value(), BadExpectedAccess);
}

TEST_CASE(expected_error)
{
    Expected<void, std::string> e{};
    CHECK(e.has_value());
    [&]() -> void { return e.value(); }(); // check value returns void and does not throw
    [&]() -> void { return *e; }();

    e = Unexpected{"fail"};
    CHECK(!e.has_value());
    CHECK(!static_cast<bool>(e));
    CHECK(e.error() == "fail");

    // modify error
    e.error() += "1";

    const auto& read{e};
    CHECK(read.error() == "fail1");
}

TEST_CASE(expected_error_rvalue)
{
    {
        Expected<int, std::unique_ptr<int>> nocopy_err{Unexpected{std::make_unique<int>(7)}};
        const auto moved{std::move(nocopy_err).error()};
        CHECK(*moved == 7);
    }
    {
        Expected<void, std::unique_ptr<int>> void_nocopy_err{Unexpected{std::make_unique<int>(9)}};
        const auto moved{std::move(void_nocopy_err).error()};
        CHECK(*moved == 9);
    }
}

TEST_CASE(unexpected_error_accessors)
{
    Unexpected u{std::make_unique<int>(-1)};
    CHECK(*u.error() == -1);

    *u.error() -= 1;
    const auto& read{u};
    CHECK(*read.error() == -2);

    const auto moved{std::move(u).error()};
    CHECK(*moved == -2);
}

TEST_CASE(expected_swap)
{
    Expected<const char*, std::unique_ptr<int>> a{Unexpected{std::make_unique<int>(-1)}};
    Expected<const char*, std::unique_ptr<int>> b{"good"};
    a.swap(b);
    CHECK(a.value() == "good");
    CHECK(*b.error() == -1);
}

TEST_SUITE_END()
