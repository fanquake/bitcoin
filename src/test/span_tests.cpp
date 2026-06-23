// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <span.h>

#include <test/util/framework.h>

#include <array>
#include <set>
#include <vector>

namespace spannable {
struct Ignore
{
    template<typename T> Ignore(T&&) {}
};
template<typename T>
bool Spannable(T&& value, decltype(std::span{value})* enable = nullptr)
{
    return true;
}
bool Spannable(Ignore)
{
    return false;
}

struct SpannableYes
{
    int* data();
    int* begin();
    int* end();
    size_t size();
};
struct SpannableNo
{
    void data();
    size_t size();
};
} // namespace spannable

using namespace spannable;

TEST_SUITE_BEGIN(span_tests)

// Make sure template std::span template deduction guides accurately enable calls to
// std::span constructor overloads that work, and disable calls to constructor overloads that
// don't work. This makes it possible to use the std::span constructor in a SFINAE
// contexts like in the Spannable function above to detect whether types are or
// aren't compatible with std::span at compile time.
TEST_CASE(span_constructor_sfinae)
{
    CHECK(Spannable(std::vector<int>{}));
    CHECK(!Spannable(std::set<int>{}));
    CHECK(!Spannable(std::vector<bool>{}));
    CHECK(Spannable(std::array<int, 3>{}));
    CHECK(Spannable(std::span<int>{}));
    CHECK(Spannable("char array"));
    CHECK(Spannable(SpannableYes{}));
    CHECK(!Spannable(SpannableNo{}));
}

TEST_SUITE_END()
