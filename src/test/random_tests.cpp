// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <random.h>

#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/time.h>

#include <test/util/framework.h>

#include <algorithm>
#include <random>

TEST_SUITE_BEGIN(random_tests)

FIXTURE_TEST_CASE(osrandom_tests, BasicTestingSetup)
{
    CHECK(Random_SanityCheck());
}

FIXTURE_TEST_CASE(fastrandom_tests_deterministic, BasicTestingSetup)
{
    // Check that deterministic FastRandomContexts are deterministic
    SeedRandomForTest(SeedRand::ZEROS);
    FastRandomContext ctx1{true};
    FastRandomContext ctx2{true};

    {
        CHECK(FastRandomContext().rand<uint64_t>() == uint64_t{9330418229102544152u});
        CHECK(FastRandomContext().rand<int>() == int{618925161});
        CHECK(FastRandomContext().randrange<std::chrono::microseconds>(1h).count() == 1271170921);
        CHECK(FastRandomContext().randrange<std::chrono::milliseconds>(1h).count() == 2803534);

        CHECK(FastRandomContext().rand<uint64_t>() == uint64_t{10170981140880778086u});
        CHECK(FastRandomContext().rand<int>() == int{1689082725});
        CHECK(FastRandomContext().randrange<std::chrono::microseconds>(1h).count() == 2464643716);
        CHECK(FastRandomContext().randrange<std::chrono::milliseconds>(1h).count() == 2312205);

        CHECK(FastRandomContext().rand<uint64_t>() == uint64_t{5689404004456455543u});
        CHECK(FastRandomContext().rand<int>() == int{785839937});
        CHECK(FastRandomContext().randrange<std::chrono::microseconds>(1h).count() == 93558804);
        CHECK(FastRandomContext().randrange<std::chrono::milliseconds>(1h).count() == 507022);
    }

    {
        constexpr SteadySeconds time_point{1s};
        FastRandomContext ctx{true};
        CHECK(7 == ctx.rand_uniform_delay(time_point, 9s).time_since_epoch().count());
        CHECK(-6 == ctx.rand_uniform_delay(time_point, -9s).time_since_epoch().count());
        CHECK(1 == ctx.rand_uniform_delay(time_point, 0s).time_since_epoch().count());
        CHECK(4652286523065884857 == ctx.rand_uniform_delay(time_point, 9223372036854775807s).time_since_epoch().count());
        CHECK(-8813961240025683129 == ctx.rand_uniform_delay(time_point, -9223372036854775807s).time_since_epoch().count());
        CHECK(26443 == ctx.rand_uniform_delay(time_point, 9h).time_since_epoch().count());
    }
    CHECK(ctx1.rand32() == ctx2.rand32());
    CHECK(ctx1.rand32() == ctx2.rand32());
    CHECK(ctx1.rand64() == ctx2.rand64());
    CHECK(ctx1.randbits(3) == ctx2.randbits(3));
    CHECK(std::ranges::equal(ctx1.randbytes<std::byte>(17), ctx2.randbytes<17>())); // check vector/array behavior symmetry
    CHECK(ctx1.rand256() == ctx2.rand256());
    CHECK(ctx1.randbits(7) == ctx2.randbits(7));
    CHECK(ctx1.randbytes(128) == ctx2.randbytes(128));
    CHECK(ctx1.rand32() == ctx2.rand32());
    CHECK(ctx1.randbits(3) == ctx2.randbits(3));
    CHECK(ctx1.rand256() == ctx2.rand256());
    CHECK(ctx1.randbytes(50) == ctx2.randbytes(50));
    {
        struct MicroClock {
            using duration = std::chrono::microseconds;
        };
        FastRandomContext ctx{true};
        // Check with clock type
        CHECK(47222 == ctx.rand_uniform_duration<MicroClock>(1s).count());
        // Check with time-point type
        CHECK(2782 == ctx.rand_uniform_duration<SteadySeconds>(9h).count());
    }
}

FIXTURE_TEST_CASE(fastrandom_tests_nondeterministic, BasicTestingSetup)
{
    // Check that a nondeterministic ones are not
    {
        CHECK(FastRandomContext().rand<uint64_t>() != uint64_t{9330418229102544152u});
        CHECK(FastRandomContext().rand<int>() != int{618925161});
        CHECK(FastRandomContext().randrange<std::chrono::microseconds>(1h).count() != 1271170921);
        CHECK(FastRandomContext().randrange<std::chrono::milliseconds>(1h).count() != 2803534);

        CHECK(FastRandomContext().rand<uint64_t>() != uint64_t{10170981140880778086u});
        CHECK(FastRandomContext().rand<int>() != int{1689082725});
        CHECK(FastRandomContext().randrange<std::chrono::microseconds>(1h).count() != 2464643716);
        CHECK(FastRandomContext().randrange<std::chrono::milliseconds>(1h).count() != 2312205);

        CHECK(FastRandomContext().rand<uint64_t>() != uint64_t{5689404004456455543u});
        CHECK(FastRandomContext().rand<int>() != int{785839937});
        CHECK(FastRandomContext().randrange<std::chrono::microseconds>(1h).count() != 93558804);
        CHECK(FastRandomContext().randrange<std::chrono::milliseconds>(1h).count() != 507022);
    }

    {
        FastRandomContext ctx3, ctx4;
        CHECK(ctx3.rand64() != ctx4.rand64()); // extremely unlikely to be equal
    }
    {
        FastRandomContext ctx3, ctx4;
        CHECK(ctx3.rand256() != ctx4.rand256());
    }
    {
        FastRandomContext ctx3, ctx4;
        CHECK(ctx3.randbytes(7) != ctx4.randbytes(7));
    }
}

FIXTURE_TEST_CASE(fastrandom_randbits, BasicTestingSetup)
{
    FastRandomContext ctx1;
    FastRandomContext ctx2;
    for (int bits = 0; bits < 63; ++bits) {
        for (int j = 0; j < 1000; ++j) {
            uint64_t rangebits = ctx1.randbits(bits);
            CHECK(rangebits >> bits == 0U);
            uint64_t range = (uint64_t{1}) << bits | rangebits;
            uint64_t rand = ctx2.randrange(range);
            CHECK(rand < range);
        }
    }
}

/** Verify that RandomMixin::randbits returns 0 and 1 for every requested bit. */
FIXTURE_TEST_CASE(randbits_test, BasicTestingSetup)
{
    FastRandomContext ctx_lens; //!< RNG for producing the lengths requested from ctx_test.
    FastRandomContext ctx_test1(true), ctx_test2(true); //!< The RNGs being tested.
    int ctx_test_bitsleft{0}; //!< (Assumed value of) ctx_test::bitbuf_len

    // Run the entire test 5 times.
    for (int i = 0; i < 5; ++i) {
        // count (first) how often it has occurred, and (second) how often it was true:
        // - for every bit position, in every requested bits count (0 + 1 + 2 + ... + 64 = 2080)
        // - for every value of ctx_test_bitsleft (0..63 = 64)
        std::vector<std::pair<uint64_t, uint64_t>> seen(2080 * 64);
        while (true) {
            // Loop 1000 times, just to not continuously check std::all_of.
            for (int j = 0; j < 1000; ++j) {
                // Decide on a number of bits to request (0 through 64, inclusive; don't use randbits/randrange).
                int bits = ctx_lens.rand64() % 65;
                // Generate that many bits.
                uint64_t gen = ctx_test1.randbits(bits);
                // For certain bits counts, also test randbits<Bits> and compare.
                uint64_t gen2;
                if (bits == 0) {
                    gen2 = ctx_test2.randbits<0>();
                } else if (bits == 1) {
                    gen2 = ctx_test2.randbits<1>();
                } else if (bits == 7) {
                    gen2 = ctx_test2.randbits<7>();
                } else if (bits == 32) {
                    gen2 = ctx_test2.randbits<32>();
                } else if (bits == 51) {
                    gen2 = ctx_test2.randbits<51>();
                } else if (bits == 64) {
                    gen2 = ctx_test2.randbits<64>();
                } else {
                    gen2 = ctx_test2.randbits(bits);
                }
                CHECK(gen == gen2);
                // Make sure the result is in range.
                if (bits < 64) CHECK(gen >> bits == 0U);
                // Mark all the seen bits in the output.
                for (int bit = 0; bit < bits; ++bit) {
                    int idx = bit + (bits * (bits - 1)) / 2 + 2080 * ctx_test_bitsleft;
                    seen[idx].first += 1;
                    seen[idx].second += (gen >> bit) & 1;
                }
                // Update ctx_test_bitself.
                if (bits > ctx_test_bitsleft) {
                    ctx_test_bitsleft = ctx_test_bitsleft + 64 - bits;
                } else {
                    ctx_test_bitsleft -= bits;
                }
            }
            // Loop until every bit position/combination is seen 242 times.
            if (std::all_of(seen.begin(), seen.end(), [](const auto& x) { return x.first >= 242; })) break;
        }
        // Check that each bit appears within 7.78 standard deviations of 50%
        // (each will fail with P < 1/(2080 * 64 * 10^9)).
        for (const auto& val : seen) {
             assert(fabs(val.first * 0.5 - val.second) < sqrt(val.first * 0.25) * 7.78);
        }
    }
}

/** Does-it-compile test for compatibility with standard library RNG interface. */
FIXTURE_TEST_CASE(stdrandom_test, BasicTestingSetup)
{
    FastRandomContext ctx;
    std::uniform_int_distribution<int> distribution(3, 9);
    for (int i = 0; i < 100; ++i) {
        int x = distribution(ctx);
        CHECK(x >= 3);
        CHECK(x <= 9);

        std::vector<int> test{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        std::shuffle(test.begin(), test.end(), ctx);
        for (int j = 1; j <= 10; ++j) {
            CHECK(std::find(test.begin(), test.end(), j) != test.end());
        }
    }
}

/** Test that Shuffle reaches every permutation with equal probability. */
FIXTURE_TEST_CASE(shuffle_stat_test, BasicTestingSetup)
{
    FastRandomContext ctx(true);
    uint32_t counts[5 * 5 * 5 * 5 * 5] = {0};
    for (int i = 0; i < 12000; ++i) {
        int data[5] = {0, 1, 2, 3, 4};
        std::shuffle(std::begin(data), std::end(data), ctx);
        int pos = data[0] + data[1] * 5 + data[2] * 25 + data[3] * 125 + data[4] * 625;
        ++counts[pos];
    }
    unsigned int sum = 0;
    double chi_score = 0.0;
    for (int i = 0; i < 5 * 5 * 5 * 5 * 5; ++i) {
        int i1 = i % 5, i2 = (i / 5) % 5, i3 = (i / 25) % 5, i4 = (i / 125) % 5, i5 = i / 625;
        uint32_t count = counts[i];
        if (i1 == i2 || i1 == i3 || i1 == i4 || i1 == i5 || i2 == i3 || i2 == i4 || i2 == i5 || i3 == i4 || i3 == i5 || i4 == i5) {
            CHECK(count == 0U);
        } else {
            chi_score += ((count - 100.0) * (count - 100.0)) / 100.0;
            CHECK(count > 50U);
            CHECK(count < 150U);
            sum += count;
        }
    }
    CHECK(chi_score > 58.1411); // 99.9999% confidence interval
    CHECK(chi_score < 210.275);
    CHECK(sum == 12000U);
}

FIXTURE_TEST_CASE(xoroshiro128plusplus_reference_values, BasicTestingSetup)
{
    // numbers generated from reference implementation
    InsecureRandomContext rng(0);
    CHECK(0x6f68e1e7e2646ee1UL == rng());
    CHECK(0xbf971b7f454094ad == rng());
    CHECK(0x48f2de556f30de38UL == rng());
    CHECK(0x6ea7c59f89bbfc75UL == rng());

    // seed with a random number
    rng.Reseed(0x1a26f3fa8546b47a);
    CHECK(0xc8dc5e08d844ac7d == rng());
    CHECK(0x5b5f1f6d499dad1bUL == rng());
    CHECK(0xbeb0031f93313d6f == rng());
    CHECK(0xbfbcf4f43a264497 == rng());
}

TEST_SUITE_END()
