// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>
#include <util/string.h>

#include <test/util/framework.h>

#include <test/util/common.h>
#include <tinyformat.h>

using namespace util;
using util::detail::CheckNumFormatSpecifiers;

TEST_SUITE_BEGIN(util_string_tests)

template <unsigned NumArgs>
void TfmFormatZeroes(const std::string& fmt)
{
    std::apply([&](auto... args) {
        (void)tfm::format(tfm::RuntimeFormat{fmt}, args...);
    }, std::array<int, NumArgs>{});
}

// Helper to allow compile-time sanity checks while providing the number of
// args directly. Normally PassFmt<sizeof...(Args)> would be used.
template <unsigned NumArgs>
void PassFmt(ConstevalFormatString<NumArgs> fmt)
{
    // Execute compile-time check again at run-time to get code coverage stats
    CHECK_NOTHROW(CheckNumFormatSpecifiers<NumArgs>(fmt.fmt));

    // If ConstevalFormatString didn't throw above, make sure tinyformat doesn't
    // throw either for the same format string and parameter count combination.
    // Proves that we have some extent of protection from runtime errors
    // (tinyformat may still throw for some type mismatches).
    CHECK_NOTHROW(TfmFormatZeroes<NumArgs>(fmt.fmt));
}
template <unsigned WrongNumArgs>
void FailFmtWithError(const char* wrong_fmt, std::string_view error)
{
    CHECK_EXCEPTION(CheckNumFormatSpecifiers<WrongNumArgs>(wrong_fmt), const char*, HasReason{error});
}

TEST_CASE(ConstevalFormatString_NumSpec)
{
    PassFmt<0>("");
    PassFmt<0>("%%");
    PassFmt<1>("%s");
    PassFmt<1>("%c");
    PassFmt<0>("%%s");
    PassFmt<0>("s%%");
    PassFmt<1>("%%%s");
    PassFmt<1>("%s%%");
    PassFmt<0>(" 1$s");
    PassFmt<1>("%1$s");
    PassFmt<1>("%1$s%1$s");
    PassFmt<2>("%2$s");
    PassFmt<2>("%2$s 4$s %2$s");
    PassFmt<129>("%129$s 999$s %2$s");
    PassFmt<1>("%02d");
    PassFmt<1>("%+2s");
    PassFmt<1>("%.6i");
    PassFmt<1>("%5.2f");
    PassFmt<1>("%5.f");
    PassFmt<1>("%.f");
    PassFmt<1>("%#x");
    PassFmt<1>("%1$5i");
    PassFmt<1>("%1$-5i");
    PassFmt<1>("%1$.5i");
    // tinyformat accepts almost any "type" spec, even '%', or '_', or '\n'.
    PassFmt<1>("%123%");
    PassFmt<1>("%123%s");
    PassFmt<1>("%_");
    PassFmt<1>("%\n");

    PassFmt<2>("%*c");
    PassFmt<2>("%+*c");
    PassFmt<2>("%.*f");
    PassFmt<3>("%*.*f");
    PassFmt<3>("%2$*3$d");
    PassFmt<3>("%2$*3$.9d");
    PassFmt<3>("%2$.*3$d");
    PassFmt<3>("%2$9.*3$d");
    PassFmt<3>("%2$+9.*3$d");
    PassFmt<4>("%3$*2$.*4$f");

    // Make sure multiple flag characters "- 0+" are accepted
    PassFmt<3>("'%- 0+*.*f'");
    PassFmt<3>("'%1$- 0+*3$.*2$f'");

    auto err_mix{"Format specifiers must be all positional or all non-positional!"};
    FailFmtWithError<1>("%s%1$s", err_mix);
    FailFmtWithError<2>("%2$*d", err_mix);
    FailFmtWithError<2>("%*2$d", err_mix);
    FailFmtWithError<2>("%.*3$d", err_mix);
    FailFmtWithError<2>("%2$.*d", err_mix);

    auto err_num{"Format specifier count must match the argument count!"};
    FailFmtWithError<1>("", err_num);
    FailFmtWithError<0>("%s", err_num);
    FailFmtWithError<2>("%s", err_num);
    FailFmtWithError<0>("%1$s", err_num);
    FailFmtWithError<2>("%1$s", err_num);
    FailFmtWithError<1>("%*c", err_num);

    auto err_0_pos{"Positional format specifier must have position of at least 1"};
    FailFmtWithError<1>("%$s", err_0_pos);
    FailFmtWithError<1>("%$", err_0_pos);
    FailFmtWithError<0>("%0$", err_0_pos);
    FailFmtWithError<0>("%0$s", err_0_pos);
    FailFmtWithError<2>("%2$*$d", err_0_pos);
    FailFmtWithError<2>("%2$*0$d", err_0_pos);
    FailFmtWithError<3>("%3$*2$.*$f", err_0_pos);
    FailFmtWithError<3>("%3$*2$.*0$f", err_0_pos);

    auto err_term{"Format specifier incorrectly terminated by end of string"};
    FailFmtWithError<1>("%", err_term);
    FailFmtWithError<1>("%9", err_term);
    FailFmtWithError<1>("%9.", err_term);
    FailFmtWithError<1>("%9.9", err_term);
    FailFmtWithError<1>("%*", err_term);
    FailFmtWithError<1>("%+*", err_term);
    FailFmtWithError<1>("%.*", err_term);
    FailFmtWithError<1>("%9.*", err_term);
    FailFmtWithError<1>("%1$", err_term);
    FailFmtWithError<1>("%1$9", err_term);
    FailFmtWithError<2>("%1$*2$", err_term);
    FailFmtWithError<2>("%1$.*2$", err_term);
    FailFmtWithError<2>("%1$9.*2$", err_term);

    // Non-parity between tinyformat and ConstevalFormatString.
    // tinyformat throws but ConstevalFormatString does not.
    CHECK_EXCEPTION(tfm::format(ConstevalFormatString<1>{"%n"}, 0), tfm::format_error,
        HasReason{"tinyformat: %n conversion spec not supported"});
    CHECK_EXCEPTION(tfm::format(ConstevalFormatString<2>{"%*s"}, "hi", "hi"), tfm::format_error,
        HasReason{"tinyformat: Cannot convert from argument type to integer for use as variable width or precision"});
    CHECK_EXCEPTION(tfm::format(ConstevalFormatString<2>{"%.*s"}, "hi", "hi"), tfm::format_error,
        HasReason{"tinyformat: Cannot convert from argument type to integer for use as variable width or precision"});

    // Ensure that tinyformat throws if format string contains wrong number
    // of specifiers. PassFmt relies on this to verify tinyformat successfully
    // formats the strings, and will need to be updated if tinyformat is changed
    // not to throw on failure.
    CHECK_EXCEPTION(TfmFormatZeroes<2>("%s"), tfm::format_error,
        HasReason{"tinyformat: Not enough conversion specifiers in format string"});
    CHECK_EXCEPTION(TfmFormatZeroes<1>("%s %s"), tfm::format_error,
        HasReason{"tinyformat: Too many conversion specifiers in format string"});
}

TEST_CASE(case_insensitive_equal_test)
{
    CHECK(!CaseInsensitiveEqual("A", "B"));
    CHECK(!CaseInsensitiveEqual("A", "b"));
    CHECK(!CaseInsensitiveEqual("a", "B"));
    CHECK(!CaseInsensitiveEqual("B", "A"));
    CHECK(!CaseInsensitiveEqual("B", "a"));
    CHECK(!CaseInsensitiveEqual("b", "A"));
    CHECK(!CaseInsensitiveEqual("A", "AA"));
    CHECK(CaseInsensitiveEqual("A-A", "a-a"));
    CHECK(CaseInsensitiveEqual("A", "A"));
    CHECK(CaseInsensitiveEqual("A", "a"));
    CHECK(CaseInsensitiveEqual("a", "a"));
    CHECK(CaseInsensitiveEqual("B", "b"));
    CHECK(CaseInsensitiveEqual("ab", "aB"));
    CHECK(CaseInsensitiveEqual("Ab", "aB"));
    CHECK(CaseInsensitiveEqual("AB", "ab"));

    // Use a character with value > 127
    // to ensure we don't trigger implicit-integer-sign-change
    CHECK(!CaseInsensitiveEqual("a", "\xe4"));
}

TEST_CASE(line_reader_test)
{
    {
        // Check three lines terminated by \n and \r\n, preserving whitespace
        std::string_view input = "once upon a time\n there was a dog \r\nwho liked food\n";
        LineReader reader(input, /*max_line_length=*/128);
        CHECK(reader.Consumed() == 0U);
        CHECK(reader.Remaining() == 51U);
        std::optional<std::string> line1{reader.ReadLine()};
        CHECK(reader.Consumed() == 17U);
        CHECK(reader.Remaining() == 34U);
        std::optional<std::string> line2{reader.ReadLine()};
        CHECK(reader.Consumed() == 36U);
        CHECK(reader.Remaining() == 15U);
        std::optional<std::string> line3{reader.ReadLine()};
        std::optional<std::string> line4{reader.ReadLine()};
        CHECK(line1);
        CHECK(line2);
        CHECK(line3);
        CHECK(!line4);
        CHECK(line1.value() == "once upon a time");
        CHECK(line2.value() == " there was a dog ");
        CHECK(line3.value() == "who liked food");
        CHECK(reader.Consumed() == 51U);
        CHECK(reader.Remaining() == 0U);
    }
    {
        // Do not exceed max_line_length + 1 while searching for \n
        // Test with 22-character line + \n + 23-character line + \n
        std::string_view input = "once upon a time there\nwas a dog who liked tea\n";

        LineReader reader1(input, /*max_line_length=*/22);
        // First line is exactly the length of max_line_length
        CHECK(reader1.ReadLine() == "once upon a time there");
        // Second line is +1 character too long
        CHECK_EXCEPTION(reader1.ReadLine(), std::runtime_error, HasReason{"max_line_length exceeded by LineReader"});

        // Increase max_line_length by 1
        LineReader reader2(input, /*max_line_length=*/23);
        // Both lines fit within limit
        CHECK(reader2.ReadLine() == "once upon a time there");
        CHECK(reader2.ReadLine() == "was a dog who liked tea");
        // End of buffer reached
        CHECK(!reader2.ReadLine());
    }
    {
        // Empty lines are empty
        std::string_view input = "\n";
        LineReader reader(input, /*max_line_length=*/1024);
        CHECK(reader.ReadLine() == "");
        CHECK(!reader.ReadLine());
    }
    {
        // Empty buffers are null
        std::string_view input;
        LineReader reader(input, /*max_line_length=*/1024);
        CHECK(!reader.ReadLine());
    }
    {
        // Don't trim any more than \r\n
        std::string_view input = "\r\r\n";
        LineReader reader(input, /*max_line_length=*/1024);
        CHECK(reader.ReadLine() == "\r");
        CHECK(!reader.ReadLine());
    }
    {
        // Even one character is too long, if it's not \n
        std::string_view input = "ab\n";
        LineReader reader(input, /*max_line_length=*/1);
        // First line is +1 character too long
        CHECK_EXCEPTION(reader.ReadLine(), std::runtime_error, HasReason{"max_line_length exceeded by LineReader"});
    }
    {
        std::string_view input = "a\nb\n";
        LineReader reader(input, /*max_line_length=*/1);
        CHECK(reader.ReadLine() == "a");
        CHECK(reader.ReadLine() == "b");
        CHECK(!reader.ReadLine());
    }
    {
        // If ReadLine fails, the iterator is reset and we can ReadLength instead
        std::string_view input = "a\nbaboon\n";
        LineReader reader(input, /*max_line_length=*/1);
        CHECK(reader.ReadLine() == "a");
        // "baboon" is too long
        CHECK_EXCEPTION(reader.ReadLine(), std::runtime_error, HasReason{"max_line_length exceeded by LineReader"});
        CHECK(reader.ReadLength(1) == "b");
        CHECK(reader.ReadLength(1) == "a");
        CHECK(reader.ReadLength(2) == "bo");
        // "on" is too long
        CHECK_EXCEPTION(reader.ReadLine(), std::runtime_error, HasReason{"max_line_length exceeded by LineReader"});
        CHECK(reader.ReadLength(1) == "o");
        CHECK(reader.ReadLine() == "n"); // now the remainder of the buffer fits in one line
        CHECK(!reader.ReadLine());
    }
    {
        // The end of the buffer (EOB) does not count as end of line \n
        std::string_view input = "once upon a time there";

        LineReader reader(input, /*max_line_length=*/22);
        // First line is exactly the length of max_line_length, but that doesn't matter because \n is missing
        CHECK(!reader.ReadLine());
        // Data can still be read using ReadLength
        CHECK(reader.ReadLength(22) == "once upon a time there");
        // End of buffer reached
        CHECK(reader.Remaining() == 0U);
    }
    {
        // Read specific number of bytes regardless of max_line_length or \n unless buffer is too short
        std::string_view input = "once upon a time\n there was a dog \r\nwho liked food";
        LineReader reader(input, /*max_line_length=*/1);
        CHECK(reader.ReadLength(0) == "");
        CHECK(reader.ReadLength(3) == "onc");
        CHECK(reader.ReadLength(8) == "e upon a");
        CHECK(reader.ReadLength(8) == " time\n t");
        CHECK_EXCEPTION(reader.ReadLength(128), std::runtime_error, HasReason{"Not enough data in buffer"});
        // After the error the iterator is reset so we can try again
        CHECK(reader.ReadLength(31) == "here was a dog \r\nwho liked food");
        // End of buffer reached
        CHECK(reader.Remaining() == 0U);
    }
}

TEST_SUITE_END()
