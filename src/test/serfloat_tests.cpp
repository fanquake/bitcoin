// Copyright (c) 2014-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hash.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/serfloat.h>
#include <serialize.h>
#include <streams.h>

#include <test/util/framework.h>

#include <cmath>
#include <limits>

TEST_SUITE_BEGIN(serfloat_tests)

namespace {

uint64_t TestDouble(double f) {
    uint64_t i = EncodeDouble(f);
    double f2 = DecodeDouble(i);
    if (std::isnan(f)) {
        // NaN is not guaranteed to round-trip exactly.
        CHECK(std::isnan(f2));
    } else {
        // Everything else is.
        CHECK(!std::isnan(f2));
        uint64_t i2 = EncodeDouble(f2);
        CHECK(f == f2);
        CHECK(i == i2);
    }
    return i;
}

} // namespace

FIXTURE_TEST_CASE(double_serfloat_tests, BasicTestingSetup) {
    // Test specific values against their expected encoding.
    CHECK(TestDouble(0.0) == 0U);
    CHECK(TestDouble(-0.0) == 0x8000000000000000ULL);
    CHECK(TestDouble(std::numeric_limits<double>::infinity()) == 0x7ff0000000000000U);
    CHECK(TestDouble(-std::numeric_limits<double>::infinity()) == 0xfff0000000000000ULL);
    CHECK(TestDouble(0.5) == 0x3fe0000000000000ULL);
    CHECK(TestDouble(1.0) == 0x3ff0000000000000ULL);
    CHECK(TestDouble(2.0) == 0x4000000000000000ULL);
    CHECK(TestDouble(4.0) == 0x4010000000000000ULL);
    CHECK(TestDouble(785.066650390625) == 0x4088888880000000ULL);
    CHECK(TestDouble(3.7243058682384174) == 0x400dcb60e0031440ULL);
    CHECK(TestDouble(91.64070592566159) == 0x4056e901536d447aULL);
    CHECK(TestDouble(-98.63087668642575) == 0xc058a860489c007aULL);
    CHECK(TestDouble(4.908737756962054) == 0x4013a28c268b2b70ULL);
    CHECK(TestDouble(77.9247330021754) == 0x40537b2ed3547804ULL);
    CHECK(TestDouble(40.24732825357566) == 0x40441fa873c43dfcULL);
    CHECK(TestDouble(71.39395607929222) == 0x4051d936938f27b6ULL);
    CHECK(TestDouble(58.80100710817612) == 0x404d668766a2bd70ULL);
    CHECK(TestDouble(-30.10665786964975) == 0xc03e1b4dee1e01b8ULL);
    CHECK(TestDouble(60.15231509068704) == 0x404e137f0f969814ULL);
    CHECK(TestDouble(-48.15848711335961) == 0xc04814494e445bc6ULL);
    CHECK(TestDouble(26.68450101125353) == 0x403aaf3b755169b0ULL);
    CHECK(TestDouble(-65.72071986604303) == 0xc0506e2046378edeULL);
    CHECK(TestDouble(17.95575825512381) == 0x4031f4ac92b0a388ULL);
    CHECK(TestDouble(-35.27171863226279) == 0xc041a2c7ad17a42aULL);
    CHECK(TestDouble(-8.58810329425124) == 0xc0212d1bdffef538ULL);
    CHECK(TestDouble(88.51393044338977) == 0x405620e43c83b1c8ULL);
    CHECK(TestDouble(48.07224932612732) == 0x4048093f77466ffcULL);
    CHECK(TestDouble(9.867348871395659e+117) == 0x586f4daeb2459b9fULL);
    CHECK(TestDouble(-1.5166424385129721e+206) == 0xeabe3bbc484bd458ULL);
    CHECK(TestDouble(-8.585156555624594e-275) == 0x8707c76eee012429ULL);
    CHECK(TestDouble(2.2794371091628822e+113) == 0x5777b2184458f4eeULL);
    CHECK(TestDouble(-1.1290476594131867e+163) == 0xe1c91893d3488bb0ULL);
    CHECK(TestDouble(9.143848423979275e-246) == 0x0d0ff76e5f2620a3ULL);
    CHECK(TestDouble(-2.8366718125941117e+81) == 0xd0d7ec7e754b394aULL);
    CHECK(TestDouble(-1.2754409481684012e+229) == 0xef80d32f8ec55342ULL);
    CHECK(TestDouble(6.000577060053642e-186) == 0x197a1be7c8209b6aULL);
    CHECK(TestDouble(2.0839423284378986e-302) == 0x014c94f8689cb0a5ULL);
    CHECK(TestDouble(-1.422140051483753e+259) == 0xf5bd99271d04bb35ULL);
    CHECK(TestDouble(-1.0593973991188853e+46) == 0xc97db0cdb72d1046ULL);
    CHECK(TestDouble(2.62945125875249e+190) == 0x67779b36366c993bULL);
    CHECK(TestDouble(-2.920377657275094e+115) == 0xd7e7b7b45908e23bULL);
    CHECK(TestDouble(9.790289014855851e-118) == 0x27a3c031cc428bccULL);
    CHECK(TestDouble(-4.629317182034961e-114) == 0xa866ccf0b753705aULL);
    CHECK(TestDouble(-1.7674605603846528e+279) == 0xf9e8ed383ffc3e25ULL);
    CHECK(TestDouble(2.5308171727712605e+120) == 0x58ef5cd55f0ec997ULL);
    CHECK(TestDouble(-1.05034156412799e+54) == 0xcb25eea1b9350fa0ULL);

    // Test extreme values
    CHECK(TestDouble(std::numeric_limits<double>::min()) == 0x10000000000000ULL);
    CHECK(TestDouble(-std::numeric_limits<double>::min()) == 0x8010000000000000ULL);
    CHECK(TestDouble(std::numeric_limits<double>::max()) == 0x7fefffffffffffffULL);
    CHECK(TestDouble(-std::numeric_limits<double>::max()) == 0xffefffffffffffffULL);
    CHECK(TestDouble(std::numeric_limits<double>::lowest()) == 0xffefffffffffffffULL);
    CHECK(TestDouble(-std::numeric_limits<double>::lowest()) == 0x7fefffffffffffffULL);
    CHECK(TestDouble(std::numeric_limits<double>::denorm_min()) == 0x1ULL);
    CHECK(TestDouble(-std::numeric_limits<double>::denorm_min()) == 0x8000000000000001ULL);
    // Note that all NaNs are encoded the same way.
    CHECK(TestDouble(std::numeric_limits<double>::quiet_NaN()) == 0x7ff8000000000000ULL);
    CHECK(TestDouble(-std::numeric_limits<double>::quiet_NaN()) == 0x7ff8000000000000ULL);
    CHECK(TestDouble(std::numeric_limits<double>::signaling_NaN()) == 0x7ff8000000000000ULL);
    CHECK(TestDouble(-std::numeric_limits<double>::signaling_NaN()) == 0x7ff8000000000000ULL);

    // Construct doubles to test from the encoding.
    static_assert(sizeof(double) == 8);
    static_assert(sizeof(uint64_t) == 8);
    for (int j = 0; j < 1000; ++j) {
        // Iterate over 9 specific bits exhaustively; the others are chosen randomly.
        // These specific bits are the sign bit, and the 2 top and bottom bits of
        // exponent and mantissa in the IEEE754 binary64 format.
        for (int x = 0; x < 512; ++x) {
            uint64_t v = m_rng.randbits(64);
            int x_pos = 0;
            for (int v_pos : {0, 1, 50, 51, 52, 53, 61, 62, 63}) {
                v &= ~(uint64_t{1} << v_pos);
                if ((x >> (x_pos++)) & 1) v |= (uint64_t{1} << v_pos);
            }
            double f;
            memcpy(&f, &v, 8);
            TestDouble(f);
        }
    }
}

/*
Python code to generate the below hashes:

    def reversed_hex(x):
        return bytes(reversed(x)).hex()

    def dsha256(x):
        return hashlib.sha256(hashlib.sha256(x).digest()).digest()

    reversed_hex(dsha256(b''.join(struct.pack('<d', x) for x in range(0,1000)))) == '43d0c82591953c4eafe114590d392676a01585d25b25d433557f0d7878b23f96'
*/
FIXTURE_TEST_CASE(doubles, BasicTestingSetup)
{
    DataStream ss{};
    // encode
    for (int i = 0; i < 1000; i++) {
        ss << EncodeDouble(i);
    }
    CHECK(Hash(ss) == uint256{"43d0c82591953c4eafe114590d392676a01585d25b25d433557f0d7878b23f96"});

    // decode
    for (int i = 0; i < 1000; i++) {
        uint64_t val;
        ss >> val;
        double j = DecodeDouble(val);
        CHECK(i == j, "decoded:" << j << " expected:" << i);
    }
}

TEST_SUITE_END()
