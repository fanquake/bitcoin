// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction_identifier.h>
#include <streams.h>
#include <test/util/common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <test/util/framework.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_BEGIN(uint256_tests)

const unsigned char R1Array[] =
    "\x9c\x52\x4a\xdb\xcf\x56\x11\x12\x2b\x29\x12\x5e\x5d\x35\xd2\xd2"
    "\x22\x81\xaa\xb5\x33\xf0\x08\x32\xd5\x56\xb1\xf9\xea\xe5\x1d\x7d";
const char R1ArrayHex[] = "7D1DE5EAF9B156D53208F033B5AA8122D2d2355d5e12292b121156cfdb4a529c";
const uint256 R1L = uint256(std::vector<unsigned char>(R1Array,R1Array+32));
const uint160 R1S = uint160(std::vector<unsigned char>(R1Array,R1Array+20));

const unsigned char R2Array[] =
    "\x70\x32\x1d\x7c\x47\xa5\x6b\x40\x26\x7e\x0a\xc3\xa6\x9c\xb6\xbf"
    "\x13\x30\x47\xa3\x19\x2d\xda\x71\x49\x13\x72\xf0\xb4\xca\x81\xd7";
const uint256 R2L = uint256(std::vector<unsigned char>(R2Array,R2Array+32));
const uint160 R2S = uint160(std::vector<unsigned char>(R2Array,R2Array+20));

const unsigned char ZeroArray[] =
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
const uint256 ZeroL = uint256(std::vector<unsigned char>(ZeroArray,ZeroArray+32));
const uint160 ZeroS = uint160(std::vector<unsigned char>(ZeroArray,ZeroArray+20));

const unsigned char OneArray[] =
    "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
const uint256 OneL = uint256(std::vector<unsigned char>(OneArray,OneArray+32));
const uint160 OneS = uint160(std::vector<unsigned char>(OneArray,OneArray+20));

const unsigned char MaxArray[] =
    "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
    "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";
const uint256 MaxL = uint256(std::vector<unsigned char>(MaxArray,MaxArray+32));
const uint160 MaxS = uint160(std::vector<unsigned char>(MaxArray,MaxArray+20));

static std::string ArrayToString(const unsigned char A[], unsigned int width)
{
    std::stringstream Stream;
    Stream << std::hex;
    for (unsigned int i = 0; i < width; ++i)
    {
        Stream<<std::setw(2)<<std::setfill('0')<<(unsigned int)A[width-i-1];
    }
    return Stream.str();
}

TEST_CASE(basics) // constructors, equality, inequality
{
    // constructor uint256(vector<char>):
    CHECK(R1L.ToString() == ArrayToString(R1Array,32));
    CHECK(R1S.ToString() == ArrayToString(R1Array,20));
    CHECK(R2L.ToString() == ArrayToString(R2Array,32));
    CHECK(R2S.ToString() == ArrayToString(R2Array,20));
    CHECK(ZeroL.ToString() == ArrayToString(ZeroArray,32));
    CHECK(ZeroS.ToString() == ArrayToString(ZeroArray,20));
    CHECK(OneL.ToString() == ArrayToString(OneArray,32));
    CHECK(OneS.ToString() == ArrayToString(OneArray,20));
    CHECK(MaxL.ToString() == ArrayToString(MaxArray,32));
    CHECK(MaxS.ToString() == ArrayToString(MaxArray,20));
    CHECK(OneL.ToString() != ArrayToString(ZeroArray,32));
    CHECK(OneS.ToString() != ArrayToString(ZeroArray,20));

    // == and !=
    CHECK(R1L != R2L); CHECK(R1S != R2S);
    CHECK(ZeroL != OneL); CHECK(ZeroS != OneS);
    CHECK(OneL != ZeroL); CHECK(OneS != ZeroS);
    CHECK(MaxL != ZeroL); CHECK(MaxS != ZeroS);

    // String Constructor and Copy Constructor
    CHECK(uint256::FromHex(R1L.ToString()).value() == R1L);
    CHECK(uint256::FromHex(R2L.ToString()).value() == R2L);
    CHECK(uint256::FromHex(ZeroL.ToString()).value() == ZeroL);
    CHECK(uint256::FromHex(OneL.ToString()).value() == OneL);
    CHECK(uint256::FromHex(MaxL.ToString()).value() == MaxL);
    CHECK(uint256::FromHex(R1ArrayHex).value() == R1L);
    CHECK(uint256(R1L) == R1L);
    CHECK(uint256(ZeroL) == ZeroL);
    CHECK(uint256(OneL) == OneL);

    CHECK(uint160::FromHex(R1S.ToString()).value() == R1S);
    CHECK(uint160::FromHex(R2S.ToString()).value() == R2S);
    CHECK(uint160::FromHex(ZeroS.ToString()).value() == ZeroS);
    CHECK(uint160::FromHex(OneS.ToString()).value() == OneS);
    CHECK(uint160::FromHex(MaxS.ToString()).value() == MaxS);
    CHECK(uint160::FromHex(std::string_view{R1ArrayHex + 24, 40}).value() == R1S);

    CHECK(uint160(R1S) == R1S);
    CHECK(uint160(ZeroS) == ZeroS);
    CHECK(uint160(OneS) == OneS);
}

TEST_CASE(comparison) // <= >= < >
{
    uint256 LastL;
    for (int i = 255; i >= 0; --i) {
        uint256 TmpL;
        *(TmpL.begin() + (i>>3)) |= 1<<(7-(i&7));
        CHECK(LastL < TmpL);
        LastL = TmpL;
    }

    CHECK(ZeroL < R1L);
    CHECK(R2L < R1L);
    CHECK(ZeroL < OneL);
    CHECK(OneL < MaxL);
    CHECK(R1L < MaxL);
    CHECK(R2L < MaxL);

    uint160 LastS;
    for (int i = 159; i >= 0; --i) {
        uint160 TmpS;
        *(TmpS.begin() + (i>>3)) |= 1<<(7-(i&7));
        CHECK(LastS < TmpS);
        LastS = TmpS;
    }
    CHECK(ZeroS < R1S);
    CHECK(R2S < R1S);
    CHECK(ZeroS < OneS);
    CHECK(OneS < MaxS);
    CHECK(R1S < MaxS);
    CHECK(R2S < MaxS);

    // Non-arithmetic uint256s compare from the beginning of their inner arrays:
    CHECK(R2L < R1L);
    // Ensure first element comparisons give the same order as above:
    CHECK(*R2L.begin() < *R1L.begin());
    // Ensure last element comparisons give a different result (swapped params):
    CHECK(*(R1L.end()-1) < *(R2L.end()-1));
    // Hex strings represent reverse-encoded bytes, with lexicographic ordering:
    CHECK(uint256{"1000000000000000000000000000000000000000000000000000000000000000"} < uint256{"0000000000000000000000000000000000000000000000000000000000000001"});
}

TEST_CASE(methods) // GetHex FromHex begin() end() size() GetLow64 GetSerializeSize, Serialize, Unserialize
{
    CHECK(R1L.GetHex() == R1L.ToString());
    CHECK(R2L.GetHex() == R2L.ToString());
    CHECK(OneL.GetHex() == OneL.ToString());
    CHECK(MaxL.GetHex() == MaxL.ToString());
    uint256 TmpL(R1L);
    CHECK(TmpL == R1L);
    CHECK(uint256::FromHex(R2L.ToString()).value() == R2L);
    CHECK(uint256::FromHex(ZeroL.ToString()).value() == uint256());

    TmpL = uint256::FromHex(R1L.ToString()).value();
    CHECK_EQUAL_RANGES(R1L, std::ranges::subrange(R1Array, R1Array + uint256::size()));
    CHECK_EQUAL_RANGES(TmpL, std::ranges::subrange(R1Array, R1Array + uint256::size()));
    CHECK_EQUAL_RANGES(R2L, std::ranges::subrange(R2Array, R2Array + uint256::size()));
    CHECK_EQUAL_RANGES(ZeroL, std::ranges::subrange(ZeroArray, ZeroArray + uint256::size()));
    CHECK_EQUAL_RANGES(OneL, std::ranges::subrange(OneArray, OneArray + uint256::size()));
    CHECK(R1L.size() == sizeof(R1L));
    CHECK(sizeof(R1L) == 32U);
    CHECK(R1L.size() == 32U);
    CHECK(R2L.size() == 32U);
    CHECK(ZeroL.size() == 32U);
    CHECK(MaxL.size() == 32U);
    CHECK(R1L.begin() + 32 == R1L.end());
    CHECK(R2L.begin() + 32 == R2L.end());
    CHECK(OneL.begin() + 32 == OneL.end());
    CHECK(MaxL.begin() + 32 == MaxL.end());
    CHECK(TmpL.begin() + 32 == TmpL.end());
    CHECK(GetSerializeSize(R1L) == 32U);
    CHECK(GetSerializeSize(ZeroL) == 32U);

    DataStream ss{};
    ss << R1L;
    CHECK(ss.str() == std::string(R1Array,R1Array+32));
    ss >> TmpL;
    CHECK(R1L == TmpL);
    ss.clear();
    ss << ZeroL;
    CHECK(ss.str() == std::string(ZeroArray,ZeroArray+32));
    ss >> TmpL;
    CHECK(ZeroL == TmpL);
    ss.clear();
    ss << MaxL;
    CHECK(ss.str() == std::string(MaxArray,MaxArray+32));
    ss >> TmpL;
    CHECK(MaxL == TmpL);
    ss.clear();

    CHECK(R1S.GetHex() == R1S.ToString());
    CHECK(R2S.GetHex() == R2S.ToString());
    CHECK(OneS.GetHex() == OneS.ToString());
    CHECK(MaxS.GetHex() == MaxS.ToString());
    uint160 TmpS(R1S);
    CHECK(TmpS == R1S);
    CHECK(uint160::FromHex(R2S.ToString()).value() == R2S);
    CHECK(uint160::FromHex(ZeroS.ToString()).value() == uint160());

    TmpS = uint160::FromHex(R1S.ToString()).value();
    CHECK_EQUAL_RANGES(R1S, std::ranges::subrange(R1Array, R1Array + uint160::size()));
    CHECK_EQUAL_RANGES(TmpS, std::ranges::subrange(R1Array, R1Array + uint160::size()));
    CHECK_EQUAL_RANGES(R2S, std::ranges::subrange(R2Array, R2Array + uint160::size()));
    CHECK_EQUAL_RANGES(ZeroS, std::ranges::subrange(ZeroArray, ZeroArray + uint160::size()));
    CHECK_EQUAL_RANGES(OneS, std::ranges::subrange(OneArray, OneArray + uint160::size()));
    CHECK(R1S.size() == sizeof(R1S));
    CHECK(sizeof(R1S) == 20U);
    CHECK(R1S.size() == 20U);
    CHECK(R2S.size() == 20U);
    CHECK(ZeroS.size() == 20U);
    CHECK(MaxS.size() == 20U);
    CHECK(R1S.begin() + 20 == R1S.end());
    CHECK(R2S.begin() + 20 == R2S.end());
    CHECK(OneS.begin() + 20 == OneS.end());
    CHECK(MaxS.begin() + 20 == MaxS.end());
    CHECK(TmpS.begin() + 20 == TmpS.end());
    CHECK(GetSerializeSize(R1S) == 20U);
    CHECK(GetSerializeSize(ZeroS) == 20U);

    ss << R1S;
    CHECK(ss.str() == std::string(R1Array,R1Array+20));
    ss >> TmpS;
    CHECK(R1S == TmpS);
    ss.clear();
    ss << ZeroS;
    CHECK(ss.str() == std::string(ZeroArray,ZeroArray+20));
    ss >> TmpS;
    CHECK(ZeroS == TmpS);
    ss.clear();
    ss << MaxS;
    CHECK(ss.str() == std::string(MaxArray,MaxArray+20));
    ss >> TmpS;
    CHECK(MaxS == TmpS);
    ss.clear();
}

/**
 * Implemented as a templated function so it can be reused by other classes that have a FromHex()
 * method that wraps base_blob::FromHex(), such as transaction_identifier::FromHex().
 */
template <typename T>
void TestFromHex()
{
    constexpr unsigned int num_chars{T::size() * 2};
    static_assert(num_chars <= 64); // this test needs to be modified to allow for more than 64 hex chars
    const std::string valid_64char_input{"0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF"};
    const auto valid_input{valid_64char_input.substr(0, num_chars)};
    {
        // check that lower and upper case hex characters are accepted
        auto valid_result{T::FromHex(valid_input)};
        REQUIRE(valid_result);
        CHECK(valid_result->ToString() == ToLower(valid_input));
    }
    {
        // check that only strings of size num_chars are accepted
        CHECK(!T::FromHex(""));
        CHECK(!T::FromHex("0"));
        CHECK(!T::FromHex(valid_input.substr(0, num_chars / 2)));
        CHECK(!T::FromHex(valid_input.substr(0, num_chars - 1)));
        CHECK(!T::FromHex(valid_input + "0"));
    }
    {
        // check that non-hex characters are not accepted
        std::string invalid_chars{R"( !"#$%&'()*+,-./:;<=>?@GHIJKLMNOPQRSTUVWXYZ[\]^_`ghijklmnopqrstuvwxyz{|}~)"};
        for (auto c : invalid_chars) {
            CHECK(!T::FromHex(valid_input.substr(0, num_chars - 1) + c));
        }
        // 0x prefixes are invalid
        std::string invalid_prefix{"0x" + valid_input};
        CHECK(!T::FromHex(std::string_view(invalid_prefix.data(), num_chars)));
        CHECK(!T::FromHex(invalid_prefix));
    }
    {
        // check that string_view length is respected
        std::string chars_68{valid_64char_input + "0123"};
        CHECK(T::FromHex(std::string_view(chars_68.data(), num_chars)).value().ToString() == ToLower(valid_input));
        CHECK(!T::FromHex(std::string_view(chars_68.data(), num_chars - 1))); // too short
        CHECK(!T::FromHex(std::string_view(chars_68.data(), num_chars + 1))); // too long
    }
}

TEST_CASE(from_hex)
{
    TestFromHex<uint160>();
    TestFromHex<uint256>();
    TestFromHex<Txid>();
    TestFromHex<Wtxid>();
}

TEST_CASE(from_user_hex)
{
    CHECK(uint256::FromUserHex("") == uint256::ZERO);
    CHECK(uint256::FromUserHex("0x") == uint256::ZERO);
    CHECK(uint256::FromUserHex("0") == uint256::ZERO);
    CHECK(uint256::FromUserHex("00") == uint256::ZERO);
    CHECK(uint256::FromUserHex("1") == uint256::ONE);
    CHECK(uint256::FromUserHex("0x10") == uint256{0x10});
    CHECK(uint256::FromUserHex("10") == uint256{0x10});
    CHECK(uint256::FromUserHex("0xFf") == uint256{0xff});
    CHECK(uint256::FromUserHex("Ff") == uint256{0xff});
    const std::string valid_hex_64{"0x0123456789abcdef0123456789abcdef0123456789ABDCEF0123456789ABCDEF"};
    REQUIRE(valid_hex_64.size() == 2U + 64U); // 0x prefix and 64 hex digits
    CHECK(uint256::FromUserHex(valid_hex_64.substr(2)).value().ToString() == ToLower(valid_hex_64.substr(2)));
    CHECK(uint256::FromUserHex(valid_hex_64.substr(0)).value().ToString() == ToLower(valid_hex_64.substr(2)));

    CHECK(!uint256::FromUserHex("0x0 "));                       // no spaces at end,
    CHECK(!uint256::FromUserHex(" 0x0"));                       // or beginning,
    CHECK(!uint256::FromUserHex("0x 0"));                       // or middle,
    CHECK(!uint256::FromUserHex(" "));                          // etc.
    CHECK(!uint256::FromUserHex("0x0ga"));                      // invalid character
    CHECK(!uint256::FromUserHex("x0"));                         // broken prefix
    CHECK(!uint256::FromUserHex("0x0x00"));                     // two prefixes not allowed
    CHECK(!uint256::FromUserHex(valid_hex_64.substr(2) + "0")); // 1 hex digit too many
    CHECK(!uint256::FromUserHex(valid_hex_64 + "a"));           // 1 hex digit too many
    CHECK(!uint256::FromUserHex(valid_hex_64 + " "));           // whitespace after max length
    CHECK(!uint256::FromUserHex(valid_hex_64 + "z"));           // invalid character after max length
}

TEST_CASE(check_ONE)
{
    uint256 one = uint256{"0000000000000000000000000000000000000000000000000000000000000001"};
    CHECK(one == uint256::ONE);
}

TEST_CASE(FromHex_vs_uint256)
{
    auto runtime_uint{uint256::FromHex("4A5E1E4BAAB89F3A32518A88C31BC87F618f76673e2cc77ab2127b7afdeda33b")};
    constexpr uint256 consteval_uint{  "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"};
    CHECK(consteval_uint == runtime_uint);
}

TEST_SUITE_END()
