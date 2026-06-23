// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/result.h>

#include <test/util/framework.h>

#include <memory>

inline bool operator==(const bilingual_str& a, const bilingual_str& b)
{
    return a.original == b.original && a.translated == b.translated;
}

inline std::ostream& operator<<(std::ostream& os, const bilingual_str& s)
{
    return os << "bilingual_str('" << s.original << "' , '" << s.translated << "')";
}

TEST_SUITE_BEGIN(result_tests)

struct NoCopy {
    NoCopy(int n) : m_n{std::make_unique<int>(n)} {}
    std::unique_ptr<int> m_n;
};

bool operator==(const NoCopy& a, const NoCopy& b)
{
    return *a.m_n == *b.m_n;
}

std::ostream& operator<<(std::ostream& os, const NoCopy& o)
{
    return os << "NoCopy(" << *o.m_n << ")";
}

util::Result<int> IntFn(int i, bool success)
{
    if (success) return i;
    return util::Error{Untranslated(strprintf("int %i error.", i))};
}

util::Result<bilingual_str> StrFn(bilingual_str s, bool success)
{
    if (success) return s;
    return util::Error{Untranslated(strprintf("str %s error.", s.original))};
}

util::Result<NoCopy> NoCopyFn(int i, bool success)
{
    if (success) return {i};
    return util::Error{Untranslated(strprintf("nocopy %i error.", i))};
}

template <typename T>
void ExpectResult(const util::Result<T>& result, bool success, const bilingual_str& str)
{
    CHECK(bool(result) == success);
    CHECK(util::ErrorString(result) == str);
}

template <typename T, typename... Args>
void ExpectSuccess(const util::Result<T>& result, const bilingual_str& str, Args&&... args)
{
    ExpectResult(result, true, str);
    CHECK(result.has_value() == true);
    T expected{std::forward<Args>(args)...};
    CHECK(result.value() == expected);
    CHECK(&result.value() == &*result);
}

template <typename T, typename... Args>
void ExpectFail(const util::Result<T>& result, const bilingual_str& str)
{
    ExpectResult(result, false, str);
}

TEST_CASE(check_returned)
{
    ExpectSuccess(IntFn(5, true), {}, 5);
    ExpectFail(IntFn(5, false), Untranslated("int 5 error."));
    ExpectSuccess(NoCopyFn(5, true), {}, 5);
    ExpectFail(NoCopyFn(5, false), Untranslated("nocopy 5 error."));
    ExpectSuccess(StrFn(Untranslated("S"), true), {}, Untranslated("S"));
    ExpectFail(StrFn(Untranslated("S"), false), Untranslated("str S error."));
}

TEST_CASE(check_value_or)
{
    CHECK(IntFn(10, true).value_or(20) == 10);
    CHECK(IntFn(10, false).value_or(20) == 20);
    CHECK(NoCopyFn(10, true).value_or(20) == 10);
    CHECK(NoCopyFn(10, false).value_or(20) == 20);
    CHECK(StrFn(Untranslated("A"), true).value_or(Untranslated("B")) == Untranslated("A"));
    CHECK(StrFn(Untranslated("A"), false).value_or(Untranslated("B")) == Untranslated("B"));
}

TEST_SUITE_END()
