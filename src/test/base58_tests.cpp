// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/data/base58_encode_decode.json.h>

#include <base58.h>
#include <test/util/json.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/vector.h>

#include <univalue.h>

#include <test/util/framework.h>

#include <string>

using namespace std::literals;
using namespace util::hex_literals;

TEST_SUITE_BEGIN(base58_tests)

// Goal: test low-level base58 encoding functionality
FIXTURE_TEST_CASE(base58_EncodeBase58, BasicTestingSetup)
{
    UniValue tests = read_json(json_tests::base58_encode_decode);
    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        const UniValue& test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 2) // Allow for extra stuff (useful for comments)
        {
            RECORD_ERROR("Bad test: " << strTest);
            continue;
        }
        std::vector<unsigned char> sourcedata = ParseHex(test[0].get_str());
        std::string base58string = test[1].get_str();
        CHECK(EncodeBase58(sourcedata) == base58string, strTest);
    }
}

// Goal: test low-level base58 decoding functionality
FIXTURE_TEST_CASE(base58_DecodeBase58, BasicTestingSetup)
{
    UniValue tests = read_json(json_tests::base58_encode_decode);
    std::vector<unsigned char> result;

    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        const UniValue& test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 2) // Allow for extra stuff (useful for comments)
        {
            RECORD_ERROR("Bad test: " << strTest);
            continue;
        }
        std::vector<unsigned char> expected = ParseHex(test[0].get_str());
        std::string base58string = test[1].get_str();
        CHECK(DecodeBase58(base58string, result, 256), strTest);
        CHECK(result.size() == expected.size(), strTest);
        CHECK(std::equal(result.begin(), result.end(), expected.begin()), strTest);
    }

    CHECK(!DecodeBase58("invalid"s, result, 100));
    CHECK(!DecodeBase58("invalid\0"s, result, 100));
    CHECK(!DecodeBase58("\0invalid"s, result, 100));

    CHECK( DecodeBase58("good"s, result, 100));
    CHECK(!DecodeBase58("bad0IOl"s, result, 100));
    CHECK(!DecodeBase58("goodbad0IOl"s, result, 100));
    CHECK(!DecodeBase58("good\0bad0IOl"s, result, 100));

    // check that DecodeBase58 skips whitespace, but still fails with unexpected non-whitespace at the end.
    CHECK(!DecodeBase58(" \t\n\v\f\r skip \r\f\v\n\t a", result, 3));
    CHECK( DecodeBase58(" \t\n\v\f\r skip \r\f\v\n\t ", result, 3));
    constexpr auto expected{"971a55"_hex_u8};
    CHECK_EQUAL_RANGES(result, expected);

    CHECK( DecodeBase58Check("3vQB7B6MrGQZaxCuFg4oh"s, result, 100));
    CHECK(!DecodeBase58Check("3vQB7B6MrGQZaxCuFg4oi"s, result, 100));
    CHECK(!DecodeBase58Check("3vQB7B6MrGQZaxCuFg4oh0IOl"s, result, 100));
    CHECK(!DecodeBase58Check("3vQB7B6MrGQZaxCuFg4oh\0" "0IOl"s, result, 100));
}

TEST_SUITE_END()
