// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>

#include <test/util/framework.h>

#include <algorithm>
#include <string>

using namespace std::literals;

TEST_SUITE_BEGIN(base32_tests)

TEST_CASE(base32_testvectors)
{
    static const std::string vstrIn[]  = {"","f","fo","foo","foob","fooba","foobar"};
    static const std::string vstrOut[] = {"","my======","mzxq====","mzxw6===","mzxw6yq=","mzxw6ytb","mzxw6ytboi======"};
    static const std::string vstrOutNoPadding[] = {"","my","mzxq","mzxw6","mzxw6yq","mzxw6ytb","mzxw6ytboi"};
    for (unsigned int i=0; i<std::size(vstrIn); i++)
    {
        std::string strEnc = EncodeBase32(vstrIn[i]);
        CHECK(strEnc == vstrOut[i]);
        strEnc = EncodeBase32(vstrIn[i], false);
        CHECK(strEnc == vstrOutNoPadding[i]);
        auto dec = DecodeBase32(vstrOut[i]);
        REQUIRE(dec);
        CHECK(std::ranges::equal(*dec, vstrIn[i]), vstrOut[i]);
    }

    CHECK(!DecodeBase32("AWSX3VPPinvalid")); // invalid size
    CHECK( DecodeBase32("AWSX3VPP")); // valid

    // Decoding strings with embedded NUL characters should fail
    CHECK(!DecodeBase32("invalid\0"sv)); // correct size, invalid due to \0
    CHECK(!DecodeBase32("AWSX3VPP\0invalid"sv)); // correct size, invalid due to \0
}

TEST_CASE(base32_padding)
{
    // Is valid without padding
    CHECK(EncodeBase32("fooba") == "mzxw6ytb");

    // Valid size
    CHECK(!DecodeBase32("========"));
    CHECK(!DecodeBase32("a======="));
    CHECK( DecodeBase32("aa======"));
    CHECK(!DecodeBase32("aaa====="));
    CHECK( DecodeBase32("aaaa===="));
    CHECK( DecodeBase32("aaaaa==="));
    CHECK(!DecodeBase32("aaaaaa=="));
    CHECK( DecodeBase32("aaaaaaa="));
}

TEST_SUITE_END()
