// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>

#include <test/util/framework.h>

#include <algorithm>
#include <string>

using namespace std::literals;

TEST_SUITE_BEGIN(base64_tests)

TEST_CASE(base64_testvectors)
{
    static const std::string vstrIn[]  = {"","f","fo","foo","foob","fooba","foobar"};
    static const std::string vstrOut[] = {"","Zg==","Zm8=","Zm9v","Zm9vYg==","Zm9vYmE=","Zm9vYmFy"};
    for (unsigned int i=0; i<std::size(vstrIn); i++)
    {
        std::string strEnc = EncodeBase64(vstrIn[i]);
        CHECK(strEnc == vstrOut[i]);
        auto dec = DecodeBase64(strEnc);
        REQUIRE(dec);
        CHECK(std::ranges::equal(*dec, vstrIn[i]), vstrOut[i]);
    }

    {
        const std::vector<uint8_t> in_u{0xff, 0x01, 0xff};
        const std::vector<std::byte> in_b{std::byte{0xff}, std::byte{0x01}, std::byte{0xff}};
        const std::string in_s{"\xff\x01\xff"};
        const std::string out_exp{"/wH/"};
        CHECK(EncodeBase64(in_u) == out_exp);
        CHECK(EncodeBase64(in_b) == out_exp);
        CHECK(EncodeBase64(in_s) == out_exp);
    }

    CHECK(DecodeBase64("nQB/pZw=")); // valid

    // Decoding strings with embedded NUL characters should fail
    CHECK(!DecodeBase64("invalid\0"sv)); // correct size, invalid due to \0
    CHECK(!DecodeBase64("nQB/pZw=\0invalid"sv));
    CHECK(!DecodeBase64("nQB/pZw=invalid\0"sv)); // invalid, padding only allowed at the end
}

TEST_CASE(base64_padding)
{
    // Is valid without padding
    CHECK(EncodeBase64("foobar") == "Zm9vYmFy");

    // Valid size
    CHECK(!DecodeBase64("===="));
    CHECK(!DecodeBase64("a==="));
    CHECK( DecodeBase64("YQ=="));
    CHECK( DecodeBase64("YWE="));
}

TEST_SUITE_END()
