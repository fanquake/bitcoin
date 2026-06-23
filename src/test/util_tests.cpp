// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <common/signmessage.h>
#include <hash.h>
#include <key.h>
#include <script/parsing.h>
#include <span.h>
#include <sync.h>
#include <test/util/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <uint256.h>
#include <univalue.h>
#include <util/bitdeque.h>
#include <util/byte_units.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/moneystr.h>
#include <util/overflow.h>
#include <util/readwritefile.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/vector.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/types.h>

#ifndef WIN32
#include <sys/wait.h>
#endif

#include <test/util/framework.h>

using namespace std::literals;
using namespace util::hex_literals;
using util::ConstevalHexDigit;
using util::Join;
using util::RemovePrefix;
using util::RemovePrefixView;
using util::ReplaceAll;
using util::Split;
using util::SplitString;
using util::TrimString;
using util::TrimStringView;

static const std::string STRING_WITH_EMBEDDED_NULL_CHAR{"1"s "\0" "1"s};

/* defined in logging.cpp */
namespace BCLog {
    std::string LogEscapeMessage(std::string_view str);
}

TEST_SUITE_BEGIN(util_tests)

namespace {
class NoCopyOrMove
{
public:
    int i;
    explicit NoCopyOrMove(int i) : i{i} { }

    NoCopyOrMove() = delete;
    NoCopyOrMove(const NoCopyOrMove&) = delete;
    NoCopyOrMove(NoCopyOrMove&&) = delete;
    NoCopyOrMove& operator=(const NoCopyOrMove&) = delete;
    NoCopyOrMove& operator=(NoCopyOrMove&&) = delete;

    operator bool() const { return i != 0; }

    int get_ip1() { return i + 1; }
    bool test()
    {
        // Check that Assume can be used within a lambda and still call methods
        [&]() { Assume(get_ip1()); }();
        return Assume(get_ip1() != 5);
    }
};
} // namespace

FIXTURE_TEST_CASE(util_check, BasicTestingSetup)
{
    // Check that Assert can forward
    const std::unique_ptr<int> p_two = Assert(std::make_unique<int>(2));
    // Check that Assert works on lvalues and rvalues
    const int two = *Assert(p_two);
    Assert(two == 2);
    Assert(true);
    // Check that Assume can be used as unary expression
    const bool result{Assume(two == 2)};
    Assert(result);

    // Check that Assert doesn't require copy/move
    NoCopyOrMove x{9};
    Assert(x).i += 3;
    Assert(x).test();

    // Check nested Asserts
    CHECK(Assert((Assert(x).test() ? 3 : 0)) == 3);

    // Check -Wdangling-gsl does not trigger when copying the int. (It would
    // trigger on "const int&")
    const int nine{*Assert(std::optional<int>{9})};
    CHECK(9 == nine);
}

FIXTURE_TEST_CASE(util_criticalsection, BasicTestingSetup)
{
    RecursiveMutex cs;

    do {
        LOCK(cs);
        break;

        RECORD_ERROR("break was swallowed!");
    } while(0);

    do {
        TRY_LOCK(cs, lockTest);
        if (lockTest) {
            CHECK(true); // Needed to suppress "Test case [...] did not check any assertions"
            break;
        }

        RECORD_ERROR("break was swallowed!");
    } while(0);
}

constexpr char HEX_PARSE_INPUT[] = "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f";
constexpr uint8_t HEX_PARSE_OUTPUT[] = {
    0x04, 0x67, 0x8a, 0xfd, 0xb0, 0xfe, 0x55, 0x48, 0x27, 0x19, 0x67, 0xf1, 0xa6, 0x71, 0x30, 0xb7,
    0x10, 0x5c, 0xd6, 0xa8, 0x28, 0xe0, 0x39, 0x09, 0xa6, 0x79, 0x62, 0xe0, 0xea, 0x1f, 0x61, 0xde,
    0xb6, 0x49, 0xf6, 0xbc, 0x3f, 0x4c, 0xef, 0x38, 0xc4, 0xf3, 0x55, 0x04, 0xe5, 0x1e, 0xc1, 0x12,
    0xde, 0x5c, 0x38, 0x4d, 0xf7, 0xba, 0x0b, 0x8d, 0x57, 0x8a, 0x4c, 0x70, 0x2b, 0x6b, 0xf1, 0x1d,
    0x5f
};
static_assert((sizeof(HEX_PARSE_INPUT) - 1) == 2 * sizeof(HEX_PARSE_OUTPUT));
FIXTURE_TEST_CASE(parse_hex, BasicTestingSetup)
{
    std::vector<unsigned char> result;

    // Basic test vector
    std::vector<unsigned char> expected(std::begin(HEX_PARSE_OUTPUT), std::end(HEX_PARSE_OUTPUT));
    constexpr std::array<std::byte, 65> hex_literal_array{operator""_hex<util::detail::Hex(HEX_PARSE_INPUT)>()};
    auto hex_literal_span{MakeUCharSpan(hex_literal_array)};
    CHECK_EQUAL_RANGES(hex_literal_span, expected);

    const std::vector<std::byte> hex_literal_vector{operator""_hex_v<util::detail::Hex(HEX_PARSE_INPUT)>()};
    auto hex_literal_vec_span = MakeUCharSpan(hex_literal_vector);
    CHECK_EQUAL_RANGES(hex_literal_vec_span, expected);

    constexpr std::array<uint8_t, 65> hex_literal_array_uint8{operator""_hex_u8<util::detail::Hex(HEX_PARSE_INPUT)>()};
    CHECK_EQUAL_RANGES(hex_literal_array_uint8, expected);

    result = operator""_hex_v_u8<util::detail::Hex(HEX_PARSE_INPUT)>();
    CHECK_EQUAL_RANGES(result, expected);

    result = ParseHex(HEX_PARSE_INPUT);
    CHECK_EQUAL_RANGES(result, expected);

    result = TryParseHex<uint8_t>(HEX_PARSE_INPUT).value();
    CHECK_EQUAL_RANGES(result, expected);

    // Spaces between bytes must be supported
    expected = {0x12, 0x34, 0x56, 0x78};
    result = ParseHex("12 34 56 78");
    CHECK_EQUAL_RANGES(result, expected);
    result = TryParseHex<uint8_t>("12 34 56 78").value();
    CHECK_EQUAL_RANGES(result, expected);

    // Leading space must be supported
    expected = {0x89, 0x34, 0x56, 0x78};
    result = ParseHex(" 89 34 56 78");
    CHECK_EQUAL_RANGES(result, expected);
    result = TryParseHex<uint8_t>(" 89 34 56 78").value();
    CHECK_EQUAL_RANGES(result, expected);

    // Mixed case and spaces are supported
    expected = {0xff, 0xaa};
    result = ParseHex("     Ff        aA    ");
    CHECK_EQUAL_RANGES(result, expected);
    result = TryParseHex<uint8_t>("     Ff        aA    ").value();
    CHECK_EQUAL_RANGES(result, expected);

    // Empty string is supported
    static_assert(""_hex.empty());
    static_assert(""_hex_u8.empty());
    CHECK(""_hex_v.size() == 0U);
    CHECK(""_hex_v_u8.size() == 0U);
    CHECK(ParseHex("").size() == 0U);
    CHECK(TryParseHex<uint8_t>("").value().size() == 0U);

    // Spaces between nibbles is treated as invalid
    CHECK(ParseHex("AAF F").size() == 0U);
    CHECK(!TryParseHex("AAF F").has_value());

    // Embedded null is treated as invalid
    const std::string with_embedded_null{" 11 "s
                                         " \0 "
                                         " 22 "s};
    CHECK(with_embedded_null.size() == 11U);
    CHECK(ParseHex(with_embedded_null).size() == 0U);
    CHECK(!TryParseHex(with_embedded_null).has_value());

    // Non-hex is treated as invalid
    CHECK(ParseHex("1234 invalid 1234").size() == 0U);
    CHECK(!TryParseHex("1234 invalid 1234").has_value());

    // Truncated input is treated as invalid
    CHECK(ParseHex("12 3").size() == 0U);
    CHECK(!TryParseHex("12 3").has_value());
}

FIXTURE_TEST_CASE(consteval_hex_digit, BasicTestingSetup)
{
    CHECK(ConstevalHexDigit('0') == 0);
    CHECK(ConstevalHexDigit('9') == 9);
    CHECK(ConstevalHexDigit('a') == 0xa);
    CHECK(ConstevalHexDigit('f') == 0xf);
}

FIXTURE_TEST_CASE(util_HexStr, BasicTestingSetup)
{
    CHECK(HexStr(HEX_PARSE_OUTPUT) == HEX_PARSE_INPUT);
    CHECK(HexStr(std::span{HEX_PARSE_OUTPUT}.last(0)) == "");
    CHECK(HexStr(std::span{HEX_PARSE_OUTPUT}.first(0)) == "");

    {
        constexpr std::string_view out_exp{"04678afdb0"};
        constexpr std::span in_s{HEX_PARSE_OUTPUT, out_exp.size() / 2};
        const std::span<const uint8_t> in_u{MakeUCharSpan(in_s)};
        const std::span<const std::byte> in_b{MakeByteSpan(in_s)};

        CHECK(HexStr(in_u) == out_exp);
        CHECK(HexStr(in_s) == out_exp);
        CHECK(HexStr(in_b) == out_exp);
    }

    {
        auto input = std::string();
        for (size_t i=0; i<256; ++i) {
            input.push_back(static_cast<char>(i));
        }

        auto hex = HexStr(input);
        REQUIRE(hex.size() == 512U);
        static constexpr auto hexmap = std::string_view("0123456789abcdef");
        for (size_t i = 0; i < 256; ++i) {
            auto upper = hexmap.find(hex[i * 2]);
            auto lower = hexmap.find(hex[i * 2 + 1]);
            REQUIRE(upper != std::string_view::npos);
            REQUIRE(lower != std::string_view::npos);
            REQUIRE(i == upper*16 + lower);
        }
    }
}

FIXTURE_TEST_CASE(span_write_bytes, BasicTestingSetup)
{
    std::array mut_arr{uint8_t{0xaa}, uint8_t{0xbb}};
    const auto mut_bytes{MakeWritableByteSpan(mut_arr)};
    mut_bytes[1] = std::byte{0x11};
    CHECK(mut_arr.at(0) == 0xaa);
    CHECK(mut_arr.at(1) == 0x11);
}

FIXTURE_TEST_CASE(util_Join, BasicTestingSetup)
{
    // Normal version
    CHECK(Join(std::vector<std::string>{}, ", ") == "");
    CHECK(Join(std::vector<std::string>{"foo"}, ", ") == "foo");
    CHECK(Join(std::vector<std::string>{"foo", "bar"}, ", ") == "foo, bar");

    // Version with unary operator
    const auto op_upper = [](const std::string& s) { return ToUpper(s); };
    CHECK(Join(std::list<std::string>{}, ", ", op_upper) == "");
    CHECK(Join(std::list<std::string>{"foo"}, ", ", op_upper) == "FOO");
    CHECK(Join(std::list<std::string>{"foo", "bar"}, ", ", op_upper) == "FOO, BAR");
}

FIXTURE_TEST_CASE(util_ReplaceAll, BasicTestingSetup)
{
    const std::string original("A test \"%s\" string '%s'.");
    auto test_replaceall = [&original](const std::string& search, const std::string& substitute, const std::string& expected) {
        auto test = original;
        ReplaceAll(test, search, substitute);
        CHECK(test == expected);
    };

    test_replaceall("", "foo", original);
    test_replaceall(original, "foo", "foo");
    test_replaceall("%s", "foo", "A test \"foo\" string 'foo'.");
    test_replaceall("\"", "foo", "A test foo%sfoo string '%s'.");
    test_replaceall("'", "foo", "A test \"%s\" string foo%sfoo.");
}

FIXTURE_TEST_CASE(util_TrimString, BasicTestingSetup)
{
    CHECK(TrimString(" foo bar ") == "foo bar");
    CHECK(TrimStringView("\t \n  \n \f\n\r\t\v\tfoo \n \f\n\r\t\v\tbar\t  \n \f\n\r\t\v\t\n ") == "foo \n \f\n\r\t\v\tbar");
    CHECK(TrimString("\t \n foo \n\tbar\t \n ") == "foo \n\tbar");
    CHECK(TrimStringView("\t \n foo \n\tbar\t \n ", "fobar") == "\t \n foo \n\tbar\t \n ");
    CHECK(TrimString("foo bar") == "foo bar");
    CHECK(TrimStringView("foo bar", "fobar") == " ");
    CHECK(TrimString(std::string("\0 foo \0 ", 8)) == std::string("\0 foo \0", 7));
    CHECK(TrimStringView(std::string(" foo ", 5)) == std::string("foo", 3));
    CHECK(TrimString(std::string("\t\t\0\0\n\n", 6)) == std::string("\0\0", 2));
    CHECK(TrimStringView(std::string("\x05\x04\x03\x02\x01\x00", 6)) == std::string("\x05\x04\x03\x02\x01\x00", 6));
    CHECK(TrimString(std::string("\x05\x04\x03\x02\x01\x00", 6), std::string("\x05\x04\x03\x02\x01", 5)) == std::string("\0", 1));
    CHECK(TrimStringView(std::string("\x05\x04\x03\x02\x01\x00", 6), std::string("\x05\x04\x03\x02\x01\x00", 6)) == "");
}

FIXTURE_TEST_CASE(util_ParseISO8601DateTime, BasicTestingSetup)
{
    CHECK(ParseISO8601DateTime("1969-12-31T23:59:59Z").value() == -1);
    CHECK(ParseISO8601DateTime("1970-01-01T00:00:00Z").value() == 0);
    CHECK(ParseISO8601DateTime("1970-01-01T00:00:01Z").value() == 1);
    CHECK(ParseISO8601DateTime("2000-01-01T00:00:01Z").value() == 946684801);
    CHECK(ParseISO8601DateTime("2011-09-30T23:36:17Z").value() == 1317425777);
    CHECK(ParseISO8601DateTime("2100-12-31T23:59:59Z").value() == 4133980799);
    CHECK(ParseISO8601DateTime("9999-12-31T23:59:59Z").value() == 253402300799);

    // Accept edge-cases, where the time overflows. They are not produced by
    // FormatISO8601DateTime, so this can be changed in the future, if needed.
    // For now, keep compatibility with the previous implementation.
    CHECK(ParseISO8601DateTime("2000-01-01T99:00:00Z").value() == 947041200);
    CHECK(ParseISO8601DateTime("2000-01-01T00:99:00Z").value() == 946690740);
    CHECK(ParseISO8601DateTime("2000-01-01T00:00:99Z").value() == 946684899);
    CHECK(ParseISO8601DateTime("2000-01-01T99:99:99Z").value() == 947047239);

    // Reject date overflows.
    CHECK(!ParseISO8601DateTime("2000-99-01T00:00:00Z"));
    CHECK(!ParseISO8601DateTime("2000-01-99T00:00:00Z"));

    // Reject out-of-range years
    CHECK(!ParseISO8601DateTime("32768-12-31T23:59:59Z"));
    CHECK(!ParseISO8601DateTime("32767-12-31T23:59:59Z"));
    CHECK(!ParseISO8601DateTime("32767-12-31T00:00:00Z"));
    CHECK(!ParseISO8601DateTime("999-12-31T00:00:00Z"));

    // Reject invalid format
    const std::string valid{"2000-01-01T00:00:01Z"};
    CHECK(ParseISO8601DateTime(valid).has_value());
    for (auto mut{0U}; mut < valid.size(); ++mut) {
        std::string invalid{valid};
        invalid[mut] = 'a';
        CHECK(!ParseISO8601DateTime(invalid));
    }
}

FIXTURE_TEST_CASE(util_FormatISO8601DateTime, BasicTestingSetup)
{
    CHECK(FormatISO8601DateTime(971890963199) == "32767-12-31T23:59:59Z");
    CHECK(FormatISO8601DateTime(971890876800) == "32767-12-31T00:00:00Z");

    CHECK(FormatISO8601DateTime(-1) == "1969-12-31T23:59:59Z");
    CHECK(FormatISO8601DateTime(0) == "1970-01-01T00:00:00Z");
    CHECK(FormatISO8601DateTime(1) == "1970-01-01T00:00:01Z");
    CHECK(FormatISO8601DateTime(946684801) == "2000-01-01T00:00:01Z");
    CHECK(FormatISO8601DateTime(1317425777) == "2011-09-30T23:36:17Z");
    CHECK(FormatISO8601DateTime(4133980799) == "2100-12-31T23:59:59Z");
    CHECK(FormatISO8601DateTime(253402300799) == "9999-12-31T23:59:59Z");
}

FIXTURE_TEST_CASE(util_FormatISO8601Date, BasicTestingSetup)
{
    CHECK(FormatISO8601Date(971890963199) == "32767-12-31");
    CHECK(FormatISO8601Date(971890876800) == "32767-12-31");

    CHECK(FormatISO8601Date(0) == "1970-01-01");
    CHECK(FormatISO8601Date(1317425777) == "2011-09-30");
}


FIXTURE_TEST_CASE(util_FormatRFC1123DateTime, BasicTestingSetup)
{
    CHECK(FormatRFC1123DateTime(std::numeric_limits<int64_t>::max()) == "");
    CHECK(FormatRFC1123DateTime(253402300800) == "");
    CHECK(FormatRFC1123DateTime(253402300799) == "Fri, 31 Dec 9999 23:59:59 GMT");
    CHECK(FormatRFC1123DateTime(253402214400) == "Fri, 31 Dec 9999 00:00:00 GMT");
    CHECK(FormatRFC1123DateTime(1717429609) == "Mon, 03 Jun 2024 15:46:49 GMT");
    CHECK(FormatRFC1123DateTime(0) == "Thu, 01 Jan 1970 00:00:00 GMT");
    CHECK(FormatRFC1123DateTime(-1) == "Wed, 31 Dec 1969 23:59:59 GMT");
    CHECK(FormatRFC1123DateTime(-1717429609) == "Sat, 31 Jul 1915 08:13:11 GMT");
    CHECK(FormatRFC1123DateTime(-62167219200) == "Sat, 01 Jan 0000 00:00:00 GMT");
    CHECK(FormatRFC1123DateTime(-62167219201) == "");
}

FIXTURE_TEST_CASE(util_FormatMoney, BasicTestingSetup)
{
    CHECK(FormatMoney(0) == "0.00");
    CHECK(FormatMoney((COIN/10000)*123456789) == "12345.6789");
    CHECK(FormatMoney(-COIN) == "-1.00");

    CHECK(FormatMoney(COIN*100000000) == "100000000.00");
    CHECK(FormatMoney(COIN*10000000) == "10000000.00");
    CHECK(FormatMoney(COIN*1000000) == "1000000.00");
    CHECK(FormatMoney(COIN*100000) == "100000.00");
    CHECK(FormatMoney(COIN*10000) == "10000.00");
    CHECK(FormatMoney(COIN*1000) == "1000.00");
    CHECK(FormatMoney(COIN*100) == "100.00");
    CHECK(FormatMoney(COIN*10) == "10.00");
    CHECK(FormatMoney(COIN) == "1.00");
    CHECK(FormatMoney(COIN/10) == "0.10");
    CHECK(FormatMoney(COIN/100) == "0.01");
    CHECK(FormatMoney(COIN/1000) == "0.001");
    CHECK(FormatMoney(COIN/10000) == "0.0001");
    CHECK(FormatMoney(COIN/100000) == "0.00001");
    CHECK(FormatMoney(COIN/1000000) == "0.000001");
    CHECK(FormatMoney(COIN/10000000) == "0.0000001");
    CHECK(FormatMoney(COIN/100000000) == "0.00000001");

    CHECK(FormatMoney(std::numeric_limits<CAmount>::max()) == "92233720368.54775807");
    CHECK(FormatMoney(std::numeric_limits<CAmount>::max() - 1) == "92233720368.54775806");
    CHECK(FormatMoney(std::numeric_limits<CAmount>::max() - 2) == "92233720368.54775805");
    CHECK(FormatMoney(std::numeric_limits<CAmount>::max() - 3) == "92233720368.54775804");
    // ...
    CHECK(FormatMoney(std::numeric_limits<CAmount>::min() + 3) == "-92233720368.54775805");
    CHECK(FormatMoney(std::numeric_limits<CAmount>::min() + 2) == "-92233720368.54775806");
    CHECK(FormatMoney(std::numeric_limits<CAmount>::min() + 1) == "-92233720368.54775807");
    CHECK(FormatMoney(std::numeric_limits<CAmount>::min()) == "-92233720368.54775808");
}

FIXTURE_TEST_CASE(util_ParseMoney, BasicTestingSetup)
{
    CHECK(ParseMoney("0.0").value() == 0);
    CHECK(ParseMoney(".").value() == 0);
    CHECK(ParseMoney("0.").value() == 0);
    CHECK(ParseMoney(".0").value() == 0);
    CHECK(ParseMoney(".6789").value() == 6789'0000);
    CHECK(ParseMoney("12345.").value() == COIN * 12345);

    CHECK(ParseMoney("12345.6789").value() == (COIN/10000)*123456789);

    CHECK(ParseMoney("10000000.00").value() == COIN*10000000);
    CHECK(ParseMoney("1000000.00").value() == COIN*1000000);
    CHECK(ParseMoney("100000.00").value() == COIN*100000);
    CHECK(ParseMoney("10000.00").value() == COIN*10000);
    CHECK(ParseMoney("1000.00").value() == COIN*1000);
    CHECK(ParseMoney("100.00").value() == COIN*100);
    CHECK(ParseMoney("10.00").value() == COIN*10);
    CHECK(ParseMoney("1.00").value() == COIN);
    CHECK(ParseMoney("1").value() == COIN);
    CHECK(ParseMoney("   1").value() == COIN);
    CHECK(ParseMoney("1   ").value() == COIN);
    CHECK(ParseMoney("  1 ").value() == COIN);
    CHECK(ParseMoney("0.1").value() == COIN/10);
    CHECK(ParseMoney("0.01").value() == COIN/100);
    CHECK(ParseMoney("0.001").value() == COIN/1000);
    CHECK(ParseMoney("0.0001").value() == COIN/10000);
    CHECK(ParseMoney("0.00001").value() == COIN/100000);
    CHECK(ParseMoney("0.000001").value() == COIN/1000000);
    CHECK(ParseMoney("0.0000001").value() == COIN/10000000);
    CHECK(ParseMoney("0.00000001").value() == COIN/100000000);
    CHECK(ParseMoney(" 0.00000001 ").value() == COIN/100000000);
    CHECK(ParseMoney("0.00000001 ").value() == COIN/100000000);
    CHECK(ParseMoney(" 0.00000001").value() == COIN/100000000);

    // Parsing amount that cannot be represented should fail
    CHECK(!ParseMoney("100000000.00"));
    CHECK(!ParseMoney("0.000000001"));

    // Parsing empty string should fail
    CHECK(!ParseMoney(""));
    CHECK(!ParseMoney(" "));
    CHECK(!ParseMoney("  "));

    // Parsing two numbers should fail
    CHECK(!ParseMoney(".."));
    CHECK(!ParseMoney("0..0"));
    CHECK(!ParseMoney("1 2"));
    CHECK(!ParseMoney(" 1 2 "));
    CHECK(!ParseMoney(" 1.2 3 "));
    CHECK(!ParseMoney(" 1 2.3 "));

    // Embedded whitespace should fail
    CHECK(!ParseMoney(" -1 .2  "));
    CHECK(!ParseMoney("  1 .2  "));
    CHECK(!ParseMoney(" +1 .2  "));

    // Attempted 63 bit overflow should fail
    CHECK(!ParseMoney("92233720368.54775808"));

    // Parsing negative amounts must fail
    CHECK(!ParseMoney("-1"));

    // Parsing strings with embedded NUL characters should fail
    CHECK(!ParseMoney("\0-1"s));
    CHECK(!ParseMoney(STRING_WITH_EMBEDDED_NULL_CHAR));
    CHECK(!ParseMoney("1\0"s));
}

FIXTURE_TEST_CASE(util_IsHex, BasicTestingSetup)
{
    CHECK(IsHex("00"));
    CHECK(IsHex("00112233445566778899aabbccddeeffAABBCCDDEEFF"));
    CHECK(IsHex("ff"));
    CHECK(IsHex("FF"));

    CHECK(!IsHex(""));
    CHECK(!IsHex("0"));
    CHECK(!IsHex("a"));
    CHECK(!IsHex("eleven"));
    CHECK(!IsHex("00xx00"));
    CHECK(!IsHex("0x0000"));
}

FIXTURE_TEST_CASE(util_seed_insecure_rand, BasicTestingSetup)
{
    SeedRandomForTest(SeedRand::ZEROS);
    for (int mod=2;mod<11;mod++)
    {
        int mask = 1;
        // Really rough binomial confidence approximation.
        int err = 30*10000./mod*sqrt((1./mod*(1-1./mod))/10000.);
        //mask is 2^ceil(log2(mod))-1
        while(mask<mod-1)mask=(mask<<1)+1;

        int count = 0;
        //How often does it get a zero from the uniform range [0,mod)?
        for (int i = 0; i < 10000; i++) {
            uint32_t rval;
            do{
                rval=m_rng.rand32()&mask;
            }while(rval>=(uint32_t)mod);
            count += rval==0;
        }
        CHECK(count<=10000/mod+err);
        CHECK(count>=10000/mod-err);
    }
}

FIXTURE_TEST_CASE(util_TimingResistantEqual, BasicTestingSetup)
{
    CHECK(TimingResistantEqual(std::string(""), std::string("")));
    CHECK(!TimingResistantEqual(std::string("abc"), std::string("")));
    CHECK(!TimingResistantEqual(std::string(""), std::string("abc")));
    CHECK(!TimingResistantEqual(std::string("a"), std::string("aa")));
    CHECK(!TimingResistantEqual(std::string("aa"), std::string("a")));
    CHECK(TimingResistantEqual(std::string("abc"), std::string("abc")));
    CHECK(!TimingResistantEqual(std::string("abc"), std::string("aba")));
}

/* Test strprintf formatting directives.
 * Put a string before and after to ensure sanity of element sizes on stack. */
#define B "check_prefix"
#define E "check_postfix"
FIXTURE_TEST_CASE(strprintf_numbers, BasicTestingSetup)
{
    int64_t s64t = -9223372036854775807LL; /* signed 64 bit test value */
    uint64_t u64t = 18446744073709551615ULL; /* unsigned 64 bit test value */
    CHECK(strprintf("%s %d %s", B, s64t, E) == B" -9223372036854775807 " E);
    CHECK(strprintf("%s %u %s", B, u64t, E) == B" 18446744073709551615 " E);
    CHECK(strprintf("%s %x %s", B, u64t, E) == B" ffffffffffffffff " E);

    size_t st = 12345678; /* unsigned size_t test value */
    ssize_t sst = -12345678; /* signed size_t test value */
    CHECK(strprintf("%s %d %s", B, sst, E) == B" -12345678 " E);
    CHECK(strprintf("%s %u %s", B, st, E) == B" 12345678 " E);
    CHECK(strprintf("%s %x %s", B, st, E) == B" bc614e " E);

    ptrdiff_t pt = 87654321; /* positive ptrdiff_t test value */
    ptrdiff_t spt = -87654321; /* negative ptrdiff_t test value */
    CHECK(strprintf("%s %d %s", B, spt, E) == B" -87654321 " E);
    CHECK(strprintf("%s %u %s", B, pt, E) == B" 87654321 " E);
    CHECK(strprintf("%s %x %s", B, pt, E) == B" 5397fb1 " E);
}
#undef B
#undef E

FIXTURE_TEST_CASE(util_mocktime, BasicTestingSetup)
{
    FakeNodeClock clock{111s};
    // Check that mock time does not change after a sleep
    for (const auto& num_sleep : {0ms, 1ms}) {
        UninterruptibleSleep(num_sleep);
        CHECK(111 == GetTime()); // Deprecated time getter
        CHECK(111 == Now<NodeSeconds>().time_since_epoch().count());
        CHECK(111 == TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()));
        CHECK(111 == TicksSinceEpoch<SecondsDouble>(Now<NodeSeconds>()));
        CHECK(111 == GetTime<std::chrono::seconds>().count());
        CHECK(111000 == GetTime<std::chrono::milliseconds>().count());
        CHECK(111000 == TicksSinceEpoch<std::chrono::milliseconds>(NodeClock::now()));
        CHECK(111000000 == GetTime<std::chrono::microseconds>().count());
    }
}

FIXTURE_TEST_CASE(util_ticksseconds, BasicTestingSetup)
{
    CHECK(TicksSeconds(0s) == 0);
    CHECK(TicksSeconds(1s) == 1);
    CHECK(TicksSeconds(999ms) == 0);
    CHECK(TicksSeconds(1000ms) == 1);
    CHECK(TicksSeconds(1500ms) == 1);
}

FIXTURE_TEST_CASE(test_IsDigit, BasicTestingSetup)
{
    CHECK(IsDigit('0') == true);
    CHECK(IsDigit('1') == true);
    CHECK(IsDigit('8') == true);
    CHECK(IsDigit('9') == true);

    CHECK(IsDigit('0' - 1) == false);
    CHECK(IsDigit('9' + 1) == false);
    CHECK(IsDigit(0) == false);
    CHECK(IsDigit(1) == false);
    CHECK(IsDigit(8) == false);
    CHECK(IsDigit(9) == false);
}

/* Check for overflow */
template <typename T>
static void TestAddMatrixOverflow()
{
    constexpr T MAXI{std::numeric_limits<T>::max()};
    CHECK(!CheckedAdd(T{1}, MAXI));
    CHECK(!CheckedAdd(MAXI, MAXI));
    CHECK(MAXI == SaturatingAdd(T{1}, MAXI));
    CHECK(MAXI == SaturatingAdd(MAXI, MAXI));

    CHECK(T{0} == CheckedAdd(T{0}, T{0}).value());
    CHECK(MAXI == CheckedAdd(T{0}, MAXI).value());
    CHECK(MAXI == CheckedAdd(T{1}, MAXI - 1).value());
    CHECK(MAXI - 1 == CheckedAdd(T{1}, MAXI - 2).value());
    CHECK(T{0} == SaturatingAdd(T{0}, T{0}));
    CHECK(MAXI == SaturatingAdd(T{0}, MAXI));
    CHECK(MAXI == SaturatingAdd(T{1}, MAXI - 1));
    CHECK(MAXI - 1 == SaturatingAdd(T{1}, MAXI - 2));
}

/* Check for overflow or underflow */
template <typename T>
static void TestAddMatrix()
{
    TestAddMatrixOverflow<T>();
    constexpr T MINI{std::numeric_limits<T>::min()};
    constexpr T MAXI{std::numeric_limits<T>::max()};
    CHECK(!CheckedAdd(T{-1}, MINI));
    CHECK(!CheckedAdd(MINI, MINI));
    CHECK(MINI == SaturatingAdd(T{-1}, MINI));
    CHECK(MINI == SaturatingAdd(MINI, MINI));

    CHECK(MINI == CheckedAdd(T{0}, MINI).value());
    CHECK(MINI == CheckedAdd(T{-1}, MINI + 1).value());
    CHECK(-1 == CheckedAdd(MINI, MAXI).value());
    CHECK(MINI + 1 == CheckedAdd(T{-1}, MINI + 2).value());
    CHECK(MINI == SaturatingAdd(T{0}, MINI));
    CHECK(MINI == SaturatingAdd(T{-1}, MINI + 1));
    CHECK(MINI + 1 == SaturatingAdd(T{-1}, MINI + 2));
    CHECK(-1 == SaturatingAdd(MINI, MAXI));
}

FIXTURE_TEST_CASE(util_overflow, BasicTestingSetup)
{
    TestAddMatrixOverflow<unsigned>();
    TestAddMatrix<signed>();
}

template <typename T>
static void RunToIntegralTests()
{
    CHECK(!ToIntegral<T>(STRING_WITH_EMBEDDED_NULL_CHAR));
    CHECK(!ToIntegral<T>(" 1"));
    CHECK(!ToIntegral<T>("1 "));
    CHECK(!ToIntegral<T>("1a"));
    CHECK(!ToIntegral<T>("1.1"));
    CHECK(!ToIntegral<T>("1.9"));
    CHECK(!ToIntegral<T>("+01.9"));
    CHECK(!ToIntegral<T>("-"));
    CHECK(!ToIntegral<T>("+"));
    CHECK(!ToIntegral<T>(" -1"));
    CHECK(!ToIntegral<T>("-1 "));
    CHECK(!ToIntegral<T>(" -1 "));
    CHECK(!ToIntegral<T>("+1"));
    CHECK(!ToIntegral<T>(" +1"));
    CHECK(!ToIntegral<T>(" +1 "));
    CHECK(!ToIntegral<T>("+-1"));
    CHECK(!ToIntegral<T>("-+1"));
    CHECK(!ToIntegral<T>("++1"));
    CHECK(!ToIntegral<T>("--1"));
    CHECK(!ToIntegral<T>(""));
    CHECK(!ToIntegral<T>("aap"));
    CHECK(!ToIntegral<T>("0x1"));
    CHECK(!ToIntegral<T>("-32482348723847471234"));
    CHECK(!ToIntegral<T>("32482348723847471234"));
}

FIXTURE_TEST_CASE(test_ToIntegral, BasicTestingSetup)
{
    CHECK(ToIntegral<int32_t>("1234").value() == 1'234);
    CHECK(ToIntegral<int32_t>("0").value() == 0);
    CHECK(ToIntegral<int32_t>("01234").value() == 1'234);
    CHECK(ToIntegral<int32_t>("00000000000000001234").value() == 1'234);
    CHECK(ToIntegral<int32_t>("-00000000000000001234").value() == -1'234);
    CHECK(ToIntegral<int32_t>("00000000000000000000").value() == 0);
    CHECK(ToIntegral<int32_t>("-00000000000000000000").value() == 0);
    CHECK(ToIntegral<int32_t>("-1234").value() == -1'234);
    CHECK(ToIntegral<int32_t>("-1").value() == -1);

    RunToIntegralTests<uint64_t>();
    RunToIntegralTests<int64_t>();
    RunToIntegralTests<uint32_t>();
    RunToIntegralTests<int32_t>();
    RunToIntegralTests<uint16_t>();
    RunToIntegralTests<int16_t>();
    RunToIntegralTests<uint8_t>();
    RunToIntegralTests<int8_t>();

    CHECK(!ToIntegral<int64_t>("-9223372036854775809"));
    CHECK(ToIntegral<int64_t>("-9223372036854775808").value() == -9'223'372'036'854'775'807LL - 1LL);
    CHECK(ToIntegral<int64_t>("9223372036854775807").value() == 9'223'372'036'854'775'807);
    CHECK(!ToIntegral<int64_t>("9223372036854775808"));

    CHECK(!ToIntegral<uint64_t>("-1"));
    CHECK(ToIntegral<uint64_t>("0").value() == 0U);
    CHECK(ToIntegral<uint64_t>("18446744073709551615").value() == 18'446'744'073'709'551'615ULL);
    CHECK(!ToIntegral<uint64_t>("18446744073709551616"));

    CHECK(!ToIntegral<int32_t>("-2147483649"));
    CHECK(ToIntegral<int32_t>("-2147483648").value() == -2'147'483'648LL);
    CHECK(ToIntegral<int32_t>("2147483647").value() == 2'147'483'647);
    CHECK(!ToIntegral<int32_t>("2147483648"));

    CHECK(!ToIntegral<uint32_t>("-1"));
    CHECK(ToIntegral<uint32_t>("0").value() == 0U);
    CHECK(ToIntegral<uint32_t>("4294967295").value() == 4'294'967'295U);
    CHECK(!ToIntegral<uint32_t>("4294967296"));

    CHECK(!ToIntegral<int16_t>("-32769"));
    CHECK(ToIntegral<int16_t>("-32768").value() == -32'768);
    CHECK(ToIntegral<int16_t>("32767").value() == 32'767);
    CHECK(!ToIntegral<int16_t>("32768"));

    CHECK(!ToIntegral<uint16_t>("-1"));
    CHECK(ToIntegral<uint16_t>("0").value() == static_cast<uint16_t>(0));
    CHECK(ToIntegral<uint16_t>("65535").value() == static_cast<uint16_t>(65'535));
    CHECK(!ToIntegral<uint16_t>("65536"));

    CHECK(!ToIntegral<int8_t>("-129"));
    CHECK(ToIntegral<int8_t>("-128").value() == -128);
    CHECK(ToIntegral<int8_t>("127").value() == 127);
    CHECK(!ToIntegral<int8_t>("128"));

    CHECK(!ToIntegral<uint8_t>("-1"));
    CHECK(ToIntegral<uint8_t>("0").value() == static_cast<uint8_t>(0));
    CHECK(ToIntegral<uint8_t>("255").value() == static_cast<uint8_t>(255));
    CHECK(!ToIntegral<uint8_t>("256"));
}

int64_t atoi64_legacy(const std::string& str)
{
    return strtoll(str.c_str(), nullptr, 10);
}

FIXTURE_TEST_CASE(test_LocaleIndependentAtoi, BasicTestingSetup)
{
    CHECK(LocaleIndependentAtoi<int32_t>("1234") == 1'234);
    CHECK(LocaleIndependentAtoi<int32_t>("0") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("01234") == 1'234);
    CHECK(LocaleIndependentAtoi<int32_t>("-1234") == -1'234);
    CHECK(LocaleIndependentAtoi<int32_t>(" 1") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>("1 ") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>("1a") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>("1.1") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>("1.9") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>("+01.9") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>("-1") == -1);
    CHECK(LocaleIndependentAtoi<int32_t>(" -1") == -1);
    CHECK(LocaleIndependentAtoi<int32_t>("-1 ") == -1);
    CHECK(LocaleIndependentAtoi<int32_t>(" -1 ") == -1);
    CHECK(LocaleIndependentAtoi<int32_t>("+1") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>(" +1") == 1);
    CHECK(LocaleIndependentAtoi<int32_t>(" +1 ") == 1);

    CHECK(LocaleIndependentAtoi<int32_t>("+-1") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("-+1") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("++1") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("--1") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("aap") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("0x1") == 0);
    CHECK(LocaleIndependentAtoi<int32_t>("-32482348723847471234") == -2'147'483'647 - 1);
    CHECK(LocaleIndependentAtoi<int32_t>("32482348723847471234") == 2'147'483'647);

    CHECK(LocaleIndependentAtoi<int64_t>("-9223372036854775809") == -9'223'372'036'854'775'807LL - 1LL);
    CHECK(LocaleIndependentAtoi<int64_t>("-9223372036854775808") == -9'223'372'036'854'775'807LL - 1LL);
    CHECK(LocaleIndependentAtoi<int64_t>("9223372036854775807") == 9'223'372'036'854'775'807);
    CHECK(LocaleIndependentAtoi<int64_t>("9223372036854775808") == 9'223'372'036'854'775'807);

    std::map<std::string, int64_t> atoi64_test_pairs = {
        {"-9223372036854775809", std::numeric_limits<int64_t>::min()},
        {"-9223372036854775808", -9'223'372'036'854'775'807LL - 1LL},
        {"9223372036854775807", 9'223'372'036'854'775'807},
        {"9223372036854775808", std::numeric_limits<int64_t>::max()},
        {"+-", 0},
        {"0x1", 0},
        {"ox1", 0},
        {"", 0},
    };

    for (const auto& pair : atoi64_test_pairs) {
        CHECK(LocaleIndependentAtoi<int64_t>(pair.first) == pair.second);
    }

    // Ensure legacy compatibility with previous versions of Bitcoin Core's atoi64
    for (const auto& pair : atoi64_test_pairs) {
        CHECK(LocaleIndependentAtoi<int64_t>(pair.first) == atoi64_legacy(pair.first));
    }

    CHECK(LocaleIndependentAtoi<uint64_t>("-1") == 0U);
    CHECK(LocaleIndependentAtoi<uint64_t>("0") == 0U);
    CHECK(LocaleIndependentAtoi<uint64_t>("18446744073709551615") == 18'446'744'073'709'551'615ULL);
    CHECK(LocaleIndependentAtoi<uint64_t>("18446744073709551616") == 18'446'744'073'709'551'615ULL);

    CHECK(LocaleIndependentAtoi<int32_t>("-2147483649") == -2'147'483'648LL);
    CHECK(LocaleIndependentAtoi<int32_t>("-2147483648") == -2'147'483'648LL);
    CHECK(LocaleIndependentAtoi<int32_t>("2147483647") == 2'147'483'647);
    CHECK(LocaleIndependentAtoi<int32_t>("2147483648") == 2'147'483'647);

    CHECK(LocaleIndependentAtoi<uint32_t>("-1") == 0U);
    CHECK(LocaleIndependentAtoi<uint32_t>("0") == 0U);
    CHECK(LocaleIndependentAtoi<uint32_t>("4294967295") == 4'294'967'295U);
    CHECK(LocaleIndependentAtoi<uint32_t>("4294967296") == 4'294'967'295U);

    CHECK(LocaleIndependentAtoi<int16_t>("-32769") == -32'768);
    CHECK(LocaleIndependentAtoi<int16_t>("-32768") == -32'768);
    CHECK(LocaleIndependentAtoi<int16_t>("32767") == 32'767);
    CHECK(LocaleIndependentAtoi<int16_t>("32768") == 32'767);

    CHECK(LocaleIndependentAtoi<uint16_t>("-1") == static_cast<uint16_t>(0));
    CHECK(LocaleIndependentAtoi<uint16_t>("0") == static_cast<uint16_t>(0));
    CHECK(LocaleIndependentAtoi<uint16_t>("65535") == static_cast<uint16_t>(65'535));
    CHECK(LocaleIndependentAtoi<uint16_t>("65536") == static_cast<uint16_t>(65'535));

    CHECK(LocaleIndependentAtoi<int8_t>("-129") == -128);
    CHECK(LocaleIndependentAtoi<int8_t>("-128") == -128);
    CHECK(LocaleIndependentAtoi<int8_t>("127") == 127);
    CHECK(LocaleIndependentAtoi<int8_t>("128") == 127);

    CHECK(LocaleIndependentAtoi<uint8_t>("-1") == static_cast<uint8_t>(0));
    CHECK(LocaleIndependentAtoi<uint8_t>("0") == static_cast<uint8_t>(0));
    CHECK(LocaleIndependentAtoi<uint8_t>("255") == static_cast<uint8_t>(255));
    CHECK(LocaleIndependentAtoi<uint8_t>("256") == static_cast<uint8_t>(255));
}

FIXTURE_TEST_CASE(test_ToIntegralHex, BasicTestingSetup)
{
    std::optional<uint64_t> n;
    // Valid values
    n = ToIntegral<uint64_t>("1234", 16);
    CHECK(*n == 0x1234U);
    n = ToIntegral<uint64_t>("a", 16);
    CHECK(*n == 0xAU);
    n = ToIntegral<uint64_t>("0000000a", 16);
    CHECK(*n == 0xAU);
    n = ToIntegral<uint64_t>("100", 16);
    CHECK(*n == 0x100U);
    n = ToIntegral<uint64_t>("DEADbeef", 16);
    CHECK(*n == 0xDEADbeefU);
    n = ToIntegral<uint64_t>("FfFfFfFf", 16);
    CHECK(*n == 0xFfFfFfFfU);
    n = ToIntegral<uint64_t>("123456789", 16);
    CHECK(*n == 0x123456789ULL);
    n = ToIntegral<uint64_t>("0", 16);
    CHECK(*n == 0U);
    n = ToIntegral<uint64_t>("FfFfFfFfFfFfFfFf", 16);
    CHECK(*n == 0xFfFfFfFfFfFfFfFfULL);
    auto signed_n = ToIntegral<int64_t>("-1", 16);
    CHECK(*signed_n == -1);
    // Invalid values
    CHECK(!ToIntegral<uint64_t>("", 16));
    CHECK(!ToIntegral<uint64_t>("-1", 16));
    CHECK(!ToIntegral<uint64_t>("10 00", 16));
    CHECK(!ToIntegral<uint64_t>("1 ", 16));
    CHECK(!ToIntegral<uint64_t>("0xAB", 16));
    CHECK(!ToIntegral<uint64_t>("FfFfFfFfFfFfFfFf0", 16));
}

FIXTURE_TEST_CASE(test_FormatParagraph, BasicTestingSetup)
{
    CHECK(FormatParagraph("", 79, 0) == "");
    CHECK(FormatParagraph("test", 79, 0) == "test");
    CHECK(FormatParagraph(" test", 79, 0) == " test");
    CHECK(FormatParagraph("test test", 79, 0) == "test test");
    CHECK(FormatParagraph("test test", 4, 0) == "test\ntest");
    CHECK(FormatParagraph("testerde test", 4, 0) == "testerde\ntest");
    CHECK(FormatParagraph("test test", 4, 4) == "test\n    test");

    // Make sure we don't indent a fully-new line following a too-long line ending
    CHECK(FormatParagraph("test test\nabc", 4, 4) == "test\n    test\nabc");

    CHECK(FormatParagraph("This_is_a_very_long_test_string_without_any_spaces_so_it_should_just_get_returned_as_is_despite_the_length until it gets here", 79) == "This_is_a_very_long_test_string_without_any_spaces_so_it_should_just_get_returned_as_is_despite_the_length\nuntil it gets here");

    // Test wrap length is exact
    CHECK(FormatParagraph("a b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 4 5 6 7 8 9 a b c de f g h i j k l m n o p", 79) == "a b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 4 5 6 7 8 9 a b c de\nf g h i j k l m n o p");
    CHECK(FormatParagraph("x\na b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 4 5 6 7 8 9 a b c de f g h i j k l m n o p", 79) == "x\na b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 4 5 6 7 8 9 a b c de\nf g h i j k l m n o p");
    // Indent should be included in length of lines
    CHECK(FormatParagraph("x\na b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 4 5 6 7 8 9 a b c de f g h i j k l m n o p q r s t u v w x y z 0 1 2 3 4 5 6 7 8 9 a b c d e fg h i j k", 79, 4) == "x\na b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 4 5 6 7 8 9 a b c de\n    f g h i j k l m n o p q r s t u v w x y z 0 1 2 3 4 5 6 7 8 9 a b c d e fg\n    h i j k");

    CHECK(FormatParagraph("This is a very long test string. This is a second sentence in the very long test string.", 79) == "This is a very long test string. This is a second sentence in the very long\ntest string.");
    CHECK(FormatParagraph("This is a very long test string.\nThis is a second sentence in the very long test string. This is a third sentence in the very long test string.", 79) == "This is a very long test string.\nThis is a second sentence in the very long test string. This is a third\nsentence in the very long test string.");
    CHECK(FormatParagraph("This is a very long test string.\n\nThis is a second sentence in the very long test string. This is a third sentence in the very long test string.", 79) == "This is a very long test string.\n\nThis is a second sentence in the very long test string. This is a third\nsentence in the very long test string.");
    CHECK(FormatParagraph("Testing that normal newlines do not get indented.\nLike here.", 79) == "Testing that normal newlines do not get indented.\nLike here.");
}

FIXTURE_TEST_CASE(test_FormatSubVersion, BasicTestingSetup)
{
    std::vector<std::string> comments;
    comments.emplace_back("comment1");
    std::vector<std::string> comments2;
    comments2.emplace_back("comment1");
    comments2.push_back(SanitizeString(std::string("Comment2; .,_?@-; !\"#$%&'()*+/<=>[]\\^`{|}~"), SAFE_CHARS_UA_COMMENT)); // Semicolon is discouraged but not forbidden by BIP-0014
    CHECK(FormatSubVersion("Test", 99900, std::vector<std::string>()) == std::string("/Test:9.99.0/"));
    CHECK(FormatSubVersion("Test", 99900, comments) == std::string("/Test:9.99.0(comment1)/"));
    CHECK(FormatSubVersion("Test", 99900, comments2) == std::string("/Test:9.99.0(comment1; Comment2; .,_?@-; )/"));
}

FIXTURE_TEST_CASE(test_ParseFixedPoint, BasicTestingSetup)
{
    int64_t amount = 0;
    CHECK(ParseFixedPoint("0", 8, &amount));
    CHECK(amount == 0LL);
    CHECK(ParseFixedPoint("1", 8, &amount));
    CHECK(amount == 100000000LL);
    CHECK(ParseFixedPoint("0.0", 8, &amount));
    CHECK(amount == 0LL);
    CHECK(ParseFixedPoint("-0.1", 8, &amount));
    CHECK(amount == -10000000LL);
    CHECK(ParseFixedPoint("1.1", 8, &amount));
    CHECK(amount == 110000000LL);
    CHECK(ParseFixedPoint("1.10000000000000000", 8, &amount));
    CHECK(amount == 110000000LL);
    CHECK(ParseFixedPoint("1.1e1", 8, &amount));
    CHECK(amount == 1100000000LL);
    CHECK(ParseFixedPoint("1.1e-1", 8, &amount));
    CHECK(amount == 11000000LL);
    CHECK(ParseFixedPoint("1000", 8, &amount));
    CHECK(amount == 100000000000LL);
    CHECK(ParseFixedPoint("-1000", 8, &amount));
    CHECK(amount == -100000000000LL);
    CHECK(ParseFixedPoint("0.00000001", 8, &amount));
    CHECK(amount == 1LL);
    CHECK(ParseFixedPoint("0.0000000100000000", 8, &amount));
    CHECK(amount == 1LL);
    CHECK(ParseFixedPoint("-0.00000001", 8, &amount));
    CHECK(amount == -1LL);
    CHECK(ParseFixedPoint("1000000000.00000001", 8, &amount));
    CHECK(amount == 100000000000000001LL);
    CHECK(ParseFixedPoint("9999999999.99999999", 8, &amount));
    CHECK(amount == 999999999999999999LL);
    CHECK(ParseFixedPoint("-9999999999.99999999", 8, &amount));
    CHECK(amount == -999999999999999999LL);

    CHECK(!ParseFixedPoint("", 8, &amount));
    CHECK(!ParseFixedPoint("-", 8, &amount));
    CHECK(!ParseFixedPoint("a-1000", 8, &amount));
    CHECK(!ParseFixedPoint("-a1000", 8, &amount));
    CHECK(!ParseFixedPoint("-1000a", 8, &amount));
    CHECK(!ParseFixedPoint("-01000", 8, &amount));
    CHECK(!ParseFixedPoint("00.1", 8, &amount));
    CHECK(!ParseFixedPoint(".1", 8, &amount));
    CHECK(!ParseFixedPoint("--0.1", 8, &amount));
    CHECK(!ParseFixedPoint("0.000000001", 8, &amount));
    CHECK(!ParseFixedPoint("-0.000000001", 8, &amount));
    CHECK(!ParseFixedPoint("0.00000001000000001", 8, &amount));
    CHECK(!ParseFixedPoint("-10000000000.00000000", 8, &amount));
    CHECK(!ParseFixedPoint("10000000000.00000000", 8, &amount));
    CHECK(!ParseFixedPoint("-10000000000.00000001", 8, &amount));
    CHECK(!ParseFixedPoint("10000000000.00000001", 8, &amount));
    CHECK(!ParseFixedPoint("-10000000000.00000009", 8, &amount));
    CHECK(!ParseFixedPoint("10000000000.00000009", 8, &amount));
    CHECK(!ParseFixedPoint("-99999999999.99999999", 8, &amount));
    CHECK(!ParseFixedPoint("99999909999.09999999", 8, &amount));
    CHECK(!ParseFixedPoint("92233720368.54775807", 8, &amount));
    CHECK(!ParseFixedPoint("92233720368.54775808", 8, &amount));
    CHECK(!ParseFixedPoint("-92233720368.54775808", 8, &amount));
    CHECK(!ParseFixedPoint("-92233720368.54775809", 8, &amount));
    CHECK(!ParseFixedPoint("1.1e", 8, &amount));
    CHECK(!ParseFixedPoint("1.1e-", 8, &amount));
    CHECK(!ParseFixedPoint("1.", 8, &amount));

    // Test with 3 decimal places for fee rates in sat/vB.
    CHECK(ParseFixedPoint("0.001", 3, &amount));
    CHECK(amount == CAmount{1});
    CHECK(!ParseFixedPoint("0.0009", 3, &amount));
    CHECK(!ParseFixedPoint("31.00100001", 3, &amount));
    CHECK(!ParseFixedPoint("31.0011", 3, &amount));
    CHECK(!ParseFixedPoint("31.99999999", 3, &amount));
    CHECK(!ParseFixedPoint("31.999999999999999999999", 3, &amount));
}

#ifndef WIN32 // Cannot do this test on WIN32 due to lack of fork()
static constexpr char LockCommand = 'L';
static constexpr char UnlockCommand = 'U';
static constexpr char ExitCommand = 'X';
enum : char {
    ResSuccess = 2, // Start with 2 to avoid accidental collision with common values 0 and 1
    ResErrorWrite,
    ResErrorLock,
    ResUnlockSuccess,
};

[[noreturn]] static void TestOtherProcess(fs::path dirname, fs::path lockname, int fd)
{
    char ch;
    while (true) {
        int rv = read(fd, &ch, 1); // Wait for command
        assert(rv == 1);
        switch (ch) {
        case LockCommand:
            ch = [&] {
                switch (util::LockDirectory(dirname, lockname)) {
                case util::LockResult::Success: return ResSuccess;
                case util::LockResult::ErrorWrite: return ResErrorWrite;
                case util::LockResult::ErrorLock: return ResErrorLock;
                } // no default case, so the compiler can warn about missing cases
                assert(false);
            }();
            rv = write(fd, &ch, 1);
            assert(rv == 1);
            break;
        case UnlockCommand:
            ReleaseDirectoryLocks();
            ch = ResUnlockSuccess; // Always succeeds
            rv = write(fd, &ch, 1);
            assert(rv == 1);
            break;
        case ExitCommand:
            close(fd);
            exit(0);
        default:
            assert(0);
        }
    }
}
#endif

FIXTURE_TEST_CASE(test_LockDirectory, BasicTestingSetup)
{
    fs::path dirname = m_args.GetDataDirBase() / "lock_dir";
    const fs::path lockname = ".lock";
#ifndef WIN32
    // Fork another process for testing before creating the lock, so that we
    // won't fork while holding the lock (which might be undefined, and is not
    // relevant as test case as that is avoided with -daemonize).
    int fd[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    pid_t pid = fork();
    if (!pid) {
        CHECK(close(fd[1]) == 0); // Child: close parent end
        TestOtherProcess(dirname, lockname, fd[0]);
    }
    CHECK(close(fd[0]) == 0); // Parent: close child end

    char ch;
    // Lock on non-existent directory should fail
    CHECK(write(fd[1], &LockCommand, 1) == 1);
    CHECK(read(fd[1], &ch, 1) == 1);
    CHECK(ch == ResErrorWrite);
#endif
    // Lock on non-existent directory should fail
    CHECK(util::LockDirectory(dirname, lockname) == util::LockResult::ErrorWrite);

    fs::create_directories(dirname);

    // Probing lock on new directory should succeed
    CHECK(util::LockDirectory(dirname, lockname, true) == util::LockResult::Success);

    // Persistent lock on new directory should succeed
    CHECK(util::LockDirectory(dirname, lockname) == util::LockResult::Success);

    // Another lock on the directory from the same thread should succeed
    CHECK(util::LockDirectory(dirname, lockname) == util::LockResult::Success);

    // Another lock on the directory from a different thread within the same process should succeed
    util::LockResult threadresult;
    std::thread thr([&] { threadresult = util::LockDirectory(dirname, lockname); });
    thr.join();
    CHECK(threadresult == util::LockResult::Success);
#ifndef WIN32
    // Try to acquire lock in child process while we're holding it, this should fail.
    CHECK(write(fd[1], &LockCommand, 1) == 1);
    CHECK(read(fd[1], &ch, 1) == 1);
    CHECK(ch == ResErrorLock);

    // Give up our lock
    ReleaseDirectoryLocks();
    // Probing lock from our side now should succeed, but not hold on to the lock.
    CHECK(util::LockDirectory(dirname, lockname, true) == util::LockResult::Success);

    // Try to acquire the lock in the child process, this should be successful.
    CHECK(write(fd[1], &LockCommand, 1) == 1);
    CHECK(read(fd[1], &ch, 1) == 1);
    CHECK(ch == ResSuccess);

    // When we try to probe the lock now, it should fail.
    CHECK(util::LockDirectory(dirname, lockname, true) == util::LockResult::ErrorLock);

    // Unlock the lock in the child process
    CHECK(write(fd[1], &UnlockCommand, 1) == 1);
    CHECK(read(fd[1], &ch, 1) == 1);
    CHECK(ch == ResUnlockSuccess);

    // When we try to probe the lock now, it should succeed.
    CHECK(util::LockDirectory(dirname, lockname, true) == util::LockResult::Success);

    // Re-lock the lock in the child process, then wait for it to exit, check
    // successful return. After that, we check that exiting the process
    // has released the lock as we would expect by probing it.
    int processstatus;
    CHECK(write(fd[1], &LockCommand, 1) == 1);
    // The following line invokes the ~CNetCleanup dtor without
    // a paired SetupNetworking call. This is acceptable as long as
    // ~CNetCleanup is a no-op for non-Windows platforms.
    CHECK(write(fd[1], &ExitCommand, 1) == 1);
    CHECK(waitpid(pid, &processstatus, 0) == pid);
    CHECK(processstatus == 0);
    CHECK(util::LockDirectory(dirname, lockname, true) == util::LockResult::Success);

    CHECK(close(fd[1]) == 0); // Close our side of the socketpair
#endif
    // Clean up
    ReleaseDirectoryLocks();
    fs::remove(dirname / lockname);
    fs::remove(dirname);
}

FIXTURE_TEST_CASE(test_ToLower, BasicTestingSetup)
{
    CHECK(ToLower('@') == '@');
    CHECK(ToLower('A') == 'a');
    CHECK(ToLower('Z') == 'z');
    CHECK(ToLower('[') == '[');
    CHECK(ToLower(0) == 0);
    CHECK(ToLower('\xff') == '\xff');

    CHECK(ToLower("") == "");
    CHECK(ToLower("#HODL") == "#hodl");
    CHECK(ToLower("\x00\xfe\xff") == "\x00\xfe\xff");
}

FIXTURE_TEST_CASE(test_ToUpper, BasicTestingSetup)
{
    CHECK(ToUpper('`') == '`');
    CHECK(ToUpper('a') == 'A');
    CHECK(ToUpper('z') == 'Z');
    CHECK(ToUpper('{') == '{');
    CHECK(ToUpper(0) == 0);
    CHECK(ToUpper('\xff') == '\xff');

    CHECK(ToUpper("") == "");
    CHECK(ToUpper("#hodl") == "#HODL");
    CHECK(ToUpper("\x00\xfe\xff") == "\x00\xfe\xff");
}

FIXTURE_TEST_CASE(test_Capitalize, BasicTestingSetup)
{
    CHECK(Capitalize("") == "");
    CHECK(Capitalize("bitcoin") == "Bitcoin");
    CHECK(Capitalize("\x00\xfe\xff") == "\x00\xfe\xff");
}

static std::string SpanToStr(const std::span<const char>& span)
{
    return std::string(span.begin(), span.end());
}

FIXTURE_TEST_CASE(test_script_parsing, BasicTestingSetup)
{
    using namespace script;
    std::string input;
    std::span<const char> sp;
    bool success;

    // Const(...): parse a constant, update span to skip it if successful
    input = "MilkToastHoney";
    sp = input;
    success = Const("", sp); // empty
    CHECK(success);
    CHECK(SpanToStr(sp) == "MilkToastHoney");

    success = Const("Milk", sp, /*skip=*/false);
    CHECK(success);
    CHECK(SpanToStr(sp) == "MilkToastHoney");

    success = Const("Milk", sp);
    CHECK(success);
    CHECK(SpanToStr(sp) == "ToastHoney");

    success = Const("Bread", sp, /*skip=*/false);
    CHECK(!success);

    success = Const("Bread", sp);
    CHECK(!success);

    success = Const("Toast", sp, /*skip=*/false);
    CHECK(success);
    CHECK(SpanToStr(sp) == "ToastHoney");

    success = Const("Toast", sp);
    CHECK(success);
    CHECK(SpanToStr(sp) == "Honey");

    success = Const("Honeybadger", sp);
    CHECK(!success);

    success = Const("Honey", sp, /*skip=*/false);
    CHECK(success);
    CHECK(SpanToStr(sp) == "Honey");

    success = Const("Honey", sp);
    CHECK(success);
    CHECK(SpanToStr(sp) == "");
    // Func(...): parse a function call, update span to argument if successful
    input = "Foo(Bar(xy,z()))";
    sp = input;

    success = Func("FooBar", sp);
    CHECK(!success);

    success = Func("Foo(", sp);
    CHECK(!success);

    success = Func("Foo", sp);
    CHECK(success);
    CHECK(SpanToStr(sp) == "Bar(xy,z())");

    success = Func("Bar", sp);
    CHECK(success);
    CHECK(SpanToStr(sp) == "xy,z()");

    success = Func("xy", sp);
    CHECK(!success);

    // Expr(...): return expression that span begins with, update span to skip it
    std::span<const char> result;

    input = "(n*(n-1))/2";
    sp = input;
    result = Expr(sp);
    CHECK(SpanToStr(result) == "(n*(n-1))/2");
    CHECK(SpanToStr(sp) == "");

    input = "foo,bar";
    sp = input;
    result = Expr(sp);
    CHECK(SpanToStr(result) == "foo");
    CHECK(SpanToStr(sp) == ",bar");

    input = "(aaaaa,bbbbb()),c";
    sp = input;
    result = Expr(sp);
    CHECK(SpanToStr(result) == "(aaaaa,bbbbb())");
    CHECK(SpanToStr(sp) == ",c");

    input = "xyz)foo";
    sp = input;
    result = Expr(sp);
    CHECK(SpanToStr(result) == "xyz");
    CHECK(SpanToStr(sp) == ")foo");

    input = "((a),(b),(c)),xxx";
    sp = input;
    result = Expr(sp);
    CHECK(SpanToStr(result) == "((a),(b),(c))");
    CHECK(SpanToStr(sp) == ",xxx");

    // Split(...): split a string on every instance of sep, return vector
    std::vector<std::span<const char>> results;

    input = "xxx";
    results = Split(input, 'x');
    CHECK(results.size() == 4U);
    CHECK(SpanToStr(results[0]) == "");
    CHECK(SpanToStr(results[1]) == "");
    CHECK(SpanToStr(results[2]) == "");
    CHECK(SpanToStr(results[3]) == "");

    input = "one#two#three";
    results = Split(input, '-');
    CHECK(results.size() == 1U);
    CHECK(SpanToStr(results[0]) == "one#two#three");

    input = "one#two#three";
    results = Split(input, '#');
    CHECK(results.size() == 3U);
    CHECK(SpanToStr(results[0]) == "one");
    CHECK(SpanToStr(results[1]) == "two");
    CHECK(SpanToStr(results[2]) == "three");

    results = Split(input, '#', /*include_sep=*/true);
    CHECK(results.size() == 3U);
    CHECK(SpanToStr(results[0]) == "one#");
    CHECK(SpanToStr(results[1]) == "two#");
    CHECK(SpanToStr(results[2]) == "three");

    input = "*foo*bar*";
    results = Split(input, '*');
    CHECK(results.size() == 4U);
    CHECK(SpanToStr(results[0]) == "");
    CHECK(SpanToStr(results[1]) == "foo");
    CHECK(SpanToStr(results[2]) == "bar");
    CHECK(SpanToStr(results[3]) == "");

    results = Split(input, '*', /*include_sep=*/true);
    CHECK(results.size() == 4U);
    CHECK(SpanToStr(results[0]) == "*");
    CHECK(SpanToStr(results[1]) == "foo*");
    CHECK(SpanToStr(results[2]) == "bar*");
    CHECK(SpanToStr(results[3]) == "");
}

FIXTURE_TEST_CASE(test_SplitString, BasicTestingSetup)
{
    // Empty string.
    {
        std::vector<std::string> result = SplitString("", '-');
        CHECK(result.size() == 1U);
        CHECK(result[0] == "");
    }

    // Empty items.
    {
        std::vector<std::string> result = SplitString("-", '-');
        CHECK(result.size() == 2U);
        CHECK(result[0] == "");
        CHECK(result[1] == "");
    }

    // More empty items.
    {
        std::vector<std::string> result = SplitString("--", '-');
        CHECK(result.size() == 3U);
        CHECK(result[0] == "");
        CHECK(result[1] == "");
        CHECK(result[2] == "");
    }

    // Separator is not present.
    {
        std::vector<std::string> result = SplitString("abc", '-');
        CHECK(result.size() == 1U);
        CHECK(result[0] == "abc");
    }

    // Basic behavior.
    {
        std::vector<std::string> result = SplitString("a-b", '-');
        CHECK(result.size() == 2U);
        CHECK(result[0] == "a");
        CHECK(result[1] == "b");
    }

    // Case-sensitivity of the separator.
    {
        std::vector<std::string> result = SplitString("AAA", 'a');
        CHECK(result.size() == 1U);
        CHECK(result[0] == "AAA");
    }

    // multiple split characters
    {
        using V = std::vector<std::string>;
        CHECK(SplitString("a,b.c:d;e", ",;") == V({"a", "b.c:d", "e"}));
        CHECK(SplitString("a,b.c:d;e", ",;:.") == V({"a", "b", "c", "d", "e"}));
        CHECK(SplitString("a,b.c:d;e", "") == V({"a,b.c:d;e"}));
        CHECK(SplitString("aaa", "bcdefg") == V({"aaa"}));
        CHECK(SplitString("x\0a,b"s, "\0"s) == V({"x", "a,b"}));
        CHECK(SplitString("x\0a,b"s, '\0') == V({"x", "a,b"}));
        CHECK(SplitString("x\0a,b"s, "\0,"s) == V({"x", "a", "b"}));
        CHECK(SplitString("abcdefg", "bcd") == V({"a", "", "", "efg"}));
    }
}

FIXTURE_TEST_CASE(test_LogEscapeMessage, BasicTestingSetup)
{
    // ASCII and UTF-8 must pass through unaltered.
    CHECK(BCLog::LogEscapeMessage("Valid log message貓") == "Valid log message貓");
    // Newlines must pass through unaltered.
    CHECK(BCLog::LogEscapeMessage("Message\n with newlines\n") == "Message\n with newlines\n");
    // Other control characters are escaped in C syntax.
    CHECK(BCLog::LogEscapeMessage("\x01\x7f Corrupted log message\x0d") == R"(\x01\x7f Corrupted log message\x0d)");
    // Embedded NULL characters are escaped too.
    const std::string NUL("O\x00O", 3);
    CHECK(BCLog::LogEscapeMessage(NUL) == R"(O\x00O)");
}

namespace {

struct Tracker
{
    //! Points to the original object (possibly itself) we moved/copied from
    const Tracker* origin;
    //! How many copies where involved between the original object and this one (moves are not counted)
    int copies{0};

    Tracker() noexcept : origin(this) {}
    Tracker(const Tracker& t) noexcept : origin(t.origin), copies(t.copies + 1) {}
    Tracker(Tracker&& t) noexcept : origin(t.origin), copies(t.copies) {}
    Tracker& operator=(const Tracker& t) noexcept
    {
        if (this != &t) {
            origin = t.origin;
            copies = t.copies + 1;
        }
        return *this;
    }
};

}

FIXTURE_TEST_CASE(test_tracked_vector, BasicTestingSetup)
{
    Tracker t1;
    Tracker t2;
    Tracker t3;

    CHECK(t1.origin == &t1);
    CHECK(t2.origin == &t2);
    CHECK(t3.origin == &t3);

    auto v1 = Vector(t1);
    CHECK(v1.size() == 1U);
    CHECK(v1[0].origin == &t1);
    CHECK(v1[0].copies == 1);

    auto v2 = Vector(std::move(t2));
    CHECK(v2.size() == 1U);
    CHECK(v2[0].origin == &t2); // NOLINT(*-use-after-move)
    CHECK(v2[0].copies == 0);

    auto v3 = Vector(t1, std::move(t2));
    CHECK(v3.size() == 2U);
    CHECK(v3[0].origin == &t1);
    CHECK(v3[1].origin == &t2); // NOLINT(*-use-after-move)
    CHECK(v3[0].copies == 1);
    CHECK(v3[1].copies == 0);

    auto v4 = Vector(std::move(v3[0]), v3[1], std::move(t3));
    CHECK(v4.size() == 3U);
    CHECK(v4[0].origin == &t1);
    CHECK(v4[1].origin == &t2);
    CHECK(v4[2].origin == &t3); // NOLINT(*-use-after-move)
    CHECK(v4[0].copies == 1);
    CHECK(v4[1].copies == 1);
    CHECK(v4[2].copies == 0);

    auto v5 = Cat(v1, v4);
    CHECK(v5.size() == 4U);
    CHECK(v5[0].origin == &t1);
    CHECK(v5[1].origin == &t1);
    CHECK(v5[2].origin == &t2);
    CHECK(v5[3].origin == &t3);
    CHECK(v5[0].copies == 2);
    CHECK(v5[1].copies == 2);
    CHECK(v5[2].copies == 2);
    CHECK(v5[3].copies == 1);

    auto v6 = Cat(std::move(v1), v3);
    CHECK(v6.size() == 3U);
    CHECK(v6[0].origin == &t1);
    CHECK(v6[1].origin == &t1);
    CHECK(v6[2].origin == &t2);
    CHECK(v6[0].copies == 1);
    CHECK(v6[1].copies == 2);
    CHECK(v6[2].copies == 1);

    auto v7 = Cat(v2, std::move(v4));
    CHECK(v7.size() == 4U);
    CHECK(v7[0].origin == &t2);
    CHECK(v7[1].origin == &t1);
    CHECK(v7[2].origin == &t2);
    CHECK(v7[3].origin == &t3);
    CHECK(v7[0].copies == 1);
    CHECK(v7[1].copies == 1);
    CHECK(v7[2].copies == 1);
    CHECK(v7[3].copies == 0);

    auto v8 = Cat(std::move(v2), std::move(v3));
    CHECK(v8.size() == 3U);
    CHECK(v8[0].origin == &t2);
    CHECK(v8[1].origin == &t1);
    CHECK(v8[2].origin == &t2);
    CHECK(v8[0].copies == 0);
    CHECK(v8[1].copies == 1);
    CHECK(v8[2].copies == 0);
}

FIXTURE_TEST_CASE(message_sign, BasicTestingSetup)
{
    const std::array<unsigned char, 32> privkey_bytes = {
        // just some random data
        // derived address from this private key: 15CRxFdyRpGZLW9w8HnHvVduizdL5jKNbs
        0xD9, 0x7F, 0x51, 0x08, 0xF1, 0x1C, 0xDA, 0x6E,
        0xEE, 0xBA, 0xAA, 0x42, 0x0F, 0xEF, 0x07, 0x26,
        0xB1, 0xF8, 0x98, 0x06, 0x0B, 0x98, 0x48, 0x9F,
        0xA3, 0x09, 0x84, 0x63, 0xC0, 0x03, 0x28, 0x66
    };

    const std::string message = "Trust no one";

    const std::string expected_signature =
        "IPojfrX2dfPnH26UegfbGQQLrdK844DlHq5157/P6h57WyuS/Qsl+h/WSVGDF4MUi4rWSswW38oimDYfNNUBUOk=";

    CKey privkey;
    std::string generated_signature;

    REQUIRE(!privkey.IsValid(), "Confirm the private key is invalid");

    CHECK(!MessageSign(privkey, message, generated_signature), "Sign with an invalid private key");

    privkey.Set(privkey_bytes.begin(), privkey_bytes.end(), true);

    REQUIRE(privkey.IsValid(), "Confirm the private key is valid");

    CHECK(MessageSign(privkey, message, generated_signature), "Sign with a valid private key");

    CHECK(expected_signature == generated_signature);
}

FIXTURE_TEST_CASE(message_verify, BasicTestingSetup)
{
    CHECK(MessageVerify(
            "invalid address",
            "signature should be irrelevant",
            "message too") == MessageVerificationResult::ERR_INVALID_ADDRESS);

    CHECK(MessageVerify(
            "3B5fQsEXEaV8v6U3ejYc8XaKXAkyQj2MjV",
            "signature should be irrelevant",
            "message too") == MessageVerificationResult::ERR_ADDRESS_NO_KEY);

    CHECK(MessageVerify(
            "1KqbBpLy5FARmTPD4VZnDDpYjkUvkr82Pm",
            "invalid signature, not in base64 encoding",
            "message should be irrelevant") == MessageVerificationResult::ERR_MALFORMED_SIGNATURE);

    CHECK(MessageVerify(
            "1KqbBpLy5FARmTPD4VZnDDpYjkUvkr82Pm",
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            "message should be irrelevant") == MessageVerificationResult::ERR_PUBKEY_NOT_RECOVERED);

    CHECK(MessageVerify(
            "15CRxFdyRpGZLW9w8HnHvVduizdL5jKNbs",
            "IPojfrX2dfPnH26UegfbGQQLrdK844DlHq5157/P6h57WyuS/Qsl+h/WSVGDF4MUi4rWSswW38oimDYfNNUBUOk=",
            "I never signed this") == MessageVerificationResult::ERR_NOT_SIGNED);

    CHECK(MessageVerify(
            "15CRxFdyRpGZLW9w8HnHvVduizdL5jKNbs",
            "IPojfrX2dfPnH26UegfbGQQLrdK844DlHq5157/P6h57WyuS/Qsl+h/WSVGDF4MUi4rWSswW38oimDYfNNUBUOk=",
            "Trust no one") == MessageVerificationResult::OK);

    CHECK(MessageVerify(
            "11canuhp9X2NocwCq7xNrQYTmUgZAnLK3",
            "IIcaIENoYW5jZWxsb3Igb24gYnJpbmsgb2Ygc2Vjb25kIGJhaWxvdXQgZm9yIGJhbmtzIAaHRtbCeDZINyavx14=",
            "Trust me") == MessageVerificationResult::OK);
}

FIXTURE_TEST_CASE(message_hash, BasicTestingSetup)
{
    const std::string unsigned_tx = "...";
    const std::string prefixed_message =
        std::string(1, (char)MESSAGE_MAGIC.length()) +
        MESSAGE_MAGIC +
        std::string(1, (char)unsigned_tx.length()) +
        unsigned_tx;

    const uint256 signature_hash = Hash(unsigned_tx);
    const uint256 message_hash1 = Hash(prefixed_message);
    const uint256 message_hash2 = MessageHash(unsigned_tx);

    CHECK(message_hash1 == message_hash2);
    CHECK(message_hash1 != signature_hash);
}

FIXTURE_TEST_CASE(remove_prefix, BasicTestingSetup)
{
    CHECK(RemovePrefix("./common/system.h", "./") == "common/system.h");
    CHECK(RemovePrefixView("foo", "foo") == "");
    CHECK(RemovePrefix("foo", "fo") == "o");
    CHECK(RemovePrefixView("foo", "f") == "oo");
    CHECK(RemovePrefix("foo", "") == "foo");
    CHECK(RemovePrefixView("fo", "foo") == "fo");
    CHECK(RemovePrefix("f", "foo") == "f");
    CHECK(RemovePrefixView("", "foo") == "");
    CHECK(RemovePrefix("", "") == "");
}

FIXTURE_TEST_CASE(util_ParseByteUnits, BasicTestingSetup)
{
    auto noop = ByteUnit::NOOP;

    // no multiplier
    CHECK(ParseByteUnits("1", noop).value() == 1U);
    CHECK(ParseByteUnits("0", noop).value() == 0U);

    CHECK(ParseByteUnits("1k", noop).value() == 1000ULL);
    CHECK(ParseByteUnits("1K", noop).value() == 1ULL << 10);

    CHECK(ParseByteUnits("2m", noop).value() == 2'000'000ULL);
    CHECK(ParseByteUnits("2M", noop).value() == 2_MiB);

    CHECK(ParseByteUnits("3g", noop).value() == 3'000'000'000ULL);
    CHECK(ParseByteUnits("3G", noop).value() == 3_GiB);

    CHECK(ParseByteUnits("4t", noop).value() == 4'000'000'000'000ULL);
    CHECK(ParseByteUnits("4T", noop).value() == 4ULL << 40);

    // check default multiplier
    CHECK(ParseByteUnits("5", ByteUnit::K).value() == 5ULL << 10);

    // NaN
    CHECK(!ParseByteUnits("", noop));
    CHECK(!ParseByteUnits("foo", noop));

    // whitespace
    CHECK(!ParseByteUnits("123m ", noop));
    CHECK(!ParseByteUnits(" 123m", noop));

    // no +-
    CHECK(!ParseByteUnits("-123m", noop));
    CHECK(!ParseByteUnits("+123m", noop));

    // zero padding
    CHECK(ParseByteUnits("020M", noop).value() == 20_MiB);

    // fractions not allowed
    CHECK(!ParseByteUnits("0.5T", noop));

    // overflow
    CHECK(!ParseByteUnits("18446744073709551615g", noop));

    // invalid unit
    CHECK(!ParseByteUnits("1x", noop));
}

FIXTURE_TEST_CASE(util_ReadBinaryFile, BasicTestingSetup)
{
    fs::path tmpfolder = m_args.GetDataDirBase();
    fs::path tmpfile = tmpfolder / "read_binary.dat";
    std::string expected_text;
    for (int i = 0; i < 30; i++) {
        expected_text += "0123456789";
    }
    {
        std::ofstream file{tmpfile.std_path()};
        file << expected_text;
    }
    {
        // read all contents in file
        auto [valid, text] = ReadBinaryFile(tmpfile);
        CHECK(valid);
        CHECK(text == expected_text);
    }
    {
        // read half contents in file
        auto [valid, text] = ReadBinaryFile(tmpfile, expected_text.size() / 2);
        CHECK(valid);
        CHECK(text == expected_text.substr(0, expected_text.size() / 2));
    }
    {
        // read from non-existent file
        fs::path invalid_file = tmpfolder / "invalid_binary.dat";
        auto [valid, text] = ReadBinaryFile(invalid_file);
        CHECK(!valid);
        CHECK(text.empty());
    }
}

FIXTURE_TEST_CASE(util_WriteBinaryFile, BasicTestingSetup)
{
    fs::path tmpfolder = m_args.GetDataDirBase();
    fs::path tmpfile = tmpfolder / "write_binary.dat";
    std::string expected_text = "bitcoin";
    auto valid = WriteBinaryFile(tmpfile, expected_text);
    std::string actual_text;
    std::ifstream file{tmpfile.std_path()};
    file >> actual_text;
    CHECK(valid);
    CHECK(actual_text == expected_text);
}

FIXTURE_TEST_CASE(clearshrink_test, BasicTestingSetup)
{
    {
        std::vector<uint8_t> v = {1, 2, 3};
        ClearShrink(v);
        CHECK(v.size() == 0U);
        CHECK(v.capacity() == 0U);
    }

    {
        std::vector<bool> v = {false, true, false, false, true, true};
        ClearShrink(v);
        CHECK(v.size() == 0U);
        CHECK(v.capacity() == 0U);
    }

    {
        std::deque<int> v = {1, 3, 3, 7};
        ClearShrink(v);
        CHECK(v.size() == 0U);
        // std::deque has no capacity() we can observe.
    }
}

template <typename T>
void TestCheckedLeftShift()
{
    constexpr auto MAX{std::numeric_limits<T>::max()};

    // Basic operations
    CHECK(CheckedLeftShift<T>(0, 1) == T{0});
    CHECK(CheckedLeftShift<T>(0, 127) == T{0});
    CHECK(CheckedLeftShift<T>(1, 1) == T{2});
    CHECK(CheckedLeftShift<T>(2, 2) == T{8});
    CHECK(CheckedLeftShift<T>(MAX >> 1, 1) == MAX - 1);

    // Max left shift
    CHECK(CheckedLeftShift<T>(1, std::numeric_limits<T>::digits - 1) == MAX / 2 + 1);

    // Overflow cases
    CHECK(!CheckedLeftShift<T>((MAX >> 1) + 1, 1));
    CHECK(!CheckedLeftShift<T>(MAX, 1));
    CHECK(!CheckedLeftShift<T>(1, std::numeric_limits<T>::digits));
    CHECK(!CheckedLeftShift<T>(1, std::numeric_limits<T>::digits + 1));

    if constexpr (std::is_signed_v<T>) {
        constexpr auto MIN{std::numeric_limits<T>::min()};
        // Negative input
        CHECK(CheckedLeftShift<T>(-1, 1) == -2);
        CHECK(CheckedLeftShift<T>((MIN >> 2), 1) == MIN / 2);
        CHECK(CheckedLeftShift<T>((MIN >> 1) + 1, 1) == MIN + 2);
        CHECK(CheckedLeftShift<T>(MIN >> 1, 1) == MIN);
        // Overflow negative
        CHECK(!CheckedLeftShift<T>((MIN >> 1) - 1, 1));
        CHECK(!CheckedLeftShift<T>(MIN >> 1, 2));
        CHECK(!CheckedLeftShift<T>(-1, 100));
    }
}

template <typename T>
void TestSaturatingLeftShift()
{
    constexpr auto MAX{std::numeric_limits<T>::max()};

    // Basic operations
    CHECK(SaturatingLeftShift<T>(0, 1) == T{0});
    CHECK(SaturatingLeftShift<T>(0, 127) == T{0});
    CHECK(SaturatingLeftShift<T>(1, 1) == T{2});
    CHECK(SaturatingLeftShift<T>(2, 2) == T{8});
    CHECK(SaturatingLeftShift<T>(MAX >> 1, 1) == MAX - 1);

    // Max left shift
    CHECK(SaturatingLeftShift<T>(1, std::numeric_limits<T>::digits - 1) == MAX / 2 + 1);

    // Saturation cases
    CHECK(SaturatingLeftShift<T>((MAX >> 1) + 1, 1) == MAX);
    CHECK(SaturatingLeftShift<T>(MAX, 1) == MAX);
    CHECK(SaturatingLeftShift<T>(1, std::numeric_limits<T>::digits) == MAX);
    CHECK(SaturatingLeftShift<T>(1, std::numeric_limits<T>::digits + 1) == MAX);

    if constexpr (std::is_signed_v<T>) {
        constexpr auto MIN{std::numeric_limits<T>::min()};
        // Negative input
        CHECK(SaturatingLeftShift<T>(-1, 1) == -2);
        CHECK(SaturatingLeftShift<T>((MIN >> 2), 1) == MIN / 2);
        CHECK(SaturatingLeftShift<T>((MIN >> 1) + 1, 1) == MIN + 2);
        CHECK(SaturatingLeftShift<T>(MIN >> 1, 1) == MIN);
        // Saturation negative
        CHECK(SaturatingLeftShift<T>((MIN >> 1) - 1, 1) == MIN);
        CHECK(SaturatingLeftShift<T>(MIN >> 1, 2) == MIN);
        CHECK(SaturatingLeftShift<T>(-1, 100) == MIN);
    }
}

FIXTURE_TEST_CASE(checked_left_shift_test, BasicTestingSetup)
{
    TestCheckedLeftShift<uint8_t>();
    TestCheckedLeftShift<int8_t>();
    TestCheckedLeftShift<size_t>();
    TestCheckedLeftShift<uint64_t>();
    TestCheckedLeftShift<int64_t>();
}

FIXTURE_TEST_CASE(saturating_left_shift_test, BasicTestingSetup)
{
    TestSaturatingLeftShift<uint8_t>();
    TestSaturatingLeftShift<int8_t>();
    TestSaturatingLeftShift<size_t>();
    TestSaturatingLeftShift<uint64_t>();
    TestSaturatingLeftShift<int64_t>();
}

template <class Int, auto bytes>
concept BraceInitializesTo = requires { Int{bytes}; };

FIXTURE_TEST_CASE(mib_string_literal_test, BasicTestingSetup)
{
    // Basic equivalences and simple arithmetic operations
    CHECK(0_MiB == 0ULL);
    CHECK(1_MiB == 1ULL << 20);
    CHECK(1_MiB == 1024ULL * 1024);
    CHECK(1_MiB == 0x100000U);
    CHECK(1_MiB == 1048576U);
    CHECK(2ULL * 1_MiB == 2ULL << 20);
    CHECK((3_MiB + 123) / double(1_MiB) == (3_MiB + 123) / 1024.0 / 1024.0);

    // Specific codebase values
    CHECK(4_MiB == 1ULL << 22);
    CHECK(8_MiB == 1ULL << 23);
    CHECK(16_MiB == 0x1000000U);
    CHECK(16_MiB == 1ULL << 24);
    CHECK(32_MiB == 0x2000000U);
    CHECK(32_MiB == 32U << 20);
    CHECK(50_MiB / 1_MiB == 50U);
    CHECK(50_MiB == 52428800U);
    CHECK(128_MiB == 0x8000000U);
    CHECK(550_MiB == 550ULL * 1024 * 1024);

    // 4095 MiB fits in uint32_t bytes. 4096 MiB requires the uint64_t return type.
    static_assert(BraceInitializesTo<uint32_t, 4095_MiB>);
    static_assert(!BraceInitializesTo<uint32_t, 4096_MiB>);
    static_assert(BraceInitializesTo<uint64_t, 4096_MiB>);
    CHECK(4095_MiB == uint32_t{4095} << 20);
    CHECK(4096_MiB == uint64_t{4096} << 20);
}

FIXTURE_TEST_CASE(ceil_div_test, BasicTestingSetup)
{
    // Type combinations used by current CeilDiv callsites.
    CHECK((std::is_same_v<decltype(CeilDiv(uint32_t{0}, 8u)), uint32_t>));
    CHECK((std::is_same_v<decltype(CeilDiv(size_t{0}, 8u)), size_t>));
    CHECK((std::is_same_v<decltype(CeilDiv(unsigned{0}, size_t{1})), size_t>));

    // `common/bloom.cpp` and `cuckoocache.h` patterns.
    CHECK(CeilDiv(uint32_t{3}, 2u) == uint32_t{2});
    CHECK(CeilDiv(uint32_t{65}, 64u) == uint32_t{2});
    CHECK(CeilDiv(uint32_t{9}, 8u) == uint32_t{2});

    // `key_io.cpp`, `rest.cpp`, `merkleblock.cpp`, `strencodings.cpp` patterns.
    CHECK(CeilDiv(size_t{9}, 8u) == size_t{2});
    CHECK(CeilDiv(size_t{10}, 3u) == size_t{4});
    CHECK(CeilDiv(size_t{11}, 5u) == size_t{3});
    CHECK(CeilDiv(size_t{41} * 8, 5u) == size_t{66});

    // `flatfile.cpp` mixed unsigned/size_t pattern.
    CHECK(CeilDiv(unsigned{10}, size_t{4}) == size_t{3});

    // `util/feefrac.h` fast-path rounding-up pattern.
    constexpr int64_t fee{12345};
    constexpr int32_t at_size{67};
    constexpr int32_t size{10};
    CHECK(CeilDiv(uint64_t(fee) * at_size, uint32_t(size)) == (uint64_t(fee) * at_size + uint32_t(size) - 1) / uint32_t(size));

    // `bitset.h` template parameter pattern.
    constexpr unsigned bits{129};
    constexpr size_t digits{std::numeric_limits<size_t>::digits};
    CHECK(CeilDiv(bits, digits) == (bits + digits - 1) / digits);

    // `serialize.h` varint scratch-buffer pattern.
    CHECK(CeilDiv(sizeof(uint64_t) * 8, 7u) == (sizeof(uint64_t) * 8 + 6) / 7);
}

FIXTURE_TEST_CASE(gib_string_literal_test, BasicTestingSetup)
{
    // Basic equivalences and simple arithmetic operations
    CHECK(0_GiB == 0ULL);
    CHECK(1_GiB == 1ULL << 30);
    CHECK(1_GiB == 1024ULL * 1024ULL * 1024ULL);
    CHECK(1_GiB == 0x40000000U);
    CHECK(1_GiB == 1073741824U);
    CHECK(1_GiB == 1_MiB * 1024);
    CHECK(1_GiB == 1024_MiB);
    CHECK((1_GiB + 123) / double(1_GiB) == (1_GiB + 123) / 1024.0 / 1024.0 / 1024.0);
    CHECK(2ULL * 1_GiB == 2ULL << 30);
    CHECK(4 * uint64_t{1_GiB} == uint64_t{4} << 30);
    CHECK(2_GiB == 2048_MiB);
    CHECK(3_GiB / 1_GiB == 3U);
    CHECK(3_GiB == 3U << 30);

    // 3 GiB fits in uint32_t bytes. 4 GiB requires the uint64_t return type.
    static_assert(BraceInitializesTo<uint32_t, 3_GiB>);
    static_assert(!BraceInitializesTo<uint32_t, 4_GiB>);
    static_assert(BraceInitializesTo<uint64_t, 4_GiB>);
    CHECK(3_GiB == uint32_t{3} << 30);
    CHECK(4_GiB == uint64_t{4} << 30);

    // Specific codebase values
    CHECK(4_GiB == 4096_MiB);
    CHECK(8_GiB == 8192_MiB);
    CHECK(16_GiB == 16384_MiB);
    CHECK(32_GiB == 32768_MiB);
}

TEST_SUITE_END()
