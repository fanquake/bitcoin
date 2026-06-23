// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/feefrac.h>
#include <random.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(feefrac_tests)

TEST_CASE(feefrac_operators)
{
    FeeFrac p1{1000, 100}, p2{500, 300};
    FeeFrac sum{1500, 400};
    FeeFrac diff{500, -200};
    FeeFrac empty{0, 0};
    FeeFrac zero_fee{0, 1}; // zero-fee allowed

    CHECK(zero_fee.EvaluateFeeDown(0) == 0);
    CHECK(zero_fee.EvaluateFeeDown(1) == 0);
    CHECK(zero_fee.EvaluateFeeDown(1000000) == 0);
    CHECK(zero_fee.EvaluateFeeDown(0x7fffffff) == 0);
    CHECK(zero_fee.EvaluateFeeUp(0) == 0);
    CHECK(zero_fee.EvaluateFeeUp(1) == 0);
    CHECK(zero_fee.EvaluateFeeUp(1000000) == 0);
    CHECK(zero_fee.EvaluateFeeUp(0x7fffffff) == 0);

    CHECK(p1.EvaluateFeeDown(0) == 0);
    CHECK(p1.EvaluateFeeDown(1) == 10);
    CHECK(p1.EvaluateFeeDown(100000000) == 1000000000);
    CHECK(p1.EvaluateFeeDown(0x7fffffff) == int64_t(0x7fffffff) * 10);
    CHECK(p1.EvaluateFeeUp(0) == 0);
    CHECK(p1.EvaluateFeeUp(1) == 10);
    CHECK(p1.EvaluateFeeUp(100000000) == 1000000000);
    CHECK(p1.EvaluateFeeUp(0x7fffffff) == int64_t(0x7fffffff) * 10);

    FeeFrac neg{-1001, 100};
    CHECK(neg.EvaluateFeeDown(0) == 0);
    CHECK(neg.EvaluateFeeDown(1) == -11);
    CHECK(neg.EvaluateFeeDown(2) == -21);
    CHECK(neg.EvaluateFeeDown(3) == -31);
    CHECK(neg.EvaluateFeeDown(100) == -1001);
    CHECK(neg.EvaluateFeeDown(101) == -1012);
    CHECK(neg.EvaluateFeeDown(100000000) == -1001000000);
    CHECK(neg.EvaluateFeeDown(100000001) == -1001000011);
    CHECK(neg.EvaluateFeeDown(0x7fffffff) == -21496311307);
    CHECK(neg.EvaluateFeeUp(0) == 0);
    CHECK(neg.EvaluateFeeUp(1) == -10);
    CHECK(neg.EvaluateFeeUp(2) == -20);
    CHECK(neg.EvaluateFeeUp(3) == -30);
    CHECK(neg.EvaluateFeeUp(100) == -1001);
    CHECK(neg.EvaluateFeeUp(101) == -1011);
    CHECK(neg.EvaluateFeeUp(100000000) == -1001000000);
    CHECK(neg.EvaluateFeeUp(100000001) == -1001000010);
    CHECK(neg.EvaluateFeeUp(0x7fffffff) == -21496311306);

    CHECK(empty == FeeFrac{}); // same as no-args

    CHECK(p1 == p1);
    CHECK(p1 + p2 == sum);
    CHECK(p1 - p2 == diff);

    FeeFrac p3{2000, 200};
    CHECK(p1 != p3); // feefracs only equal if both fee and size are same
    CHECK(p2 != p3);

    FeeFrac p4{3000, 300};
    CHECK(p1 == p4-p3);
    CHECK(p1 + p3 == p4);

    // Fee-rate comparison
    CHECK(ByRatioNegSize{p1} > ByRatioNegSize{p2});
    CHECK(ByRatioNegSize{p1} >= ByRatioNegSize{p2});
    CHECK(ByRatioNegSize{p1} >= ByRatioNegSize{p4-p3});
    CHECK(!(ByRatio{p1} > ByRatio{p3})); // not strictly better
    CHECK(ByRatio{p1} > ByRatio{p2}); // strictly greater feerate

    CHECK(ByRatioNegSize{p2} < ByRatioNegSize{p1});
    CHECK(ByRatioNegSize{p2} <= ByRatioNegSize{p1});
    CHECK(ByRatioNegSize{p1} <= ByRatioNegSize{p4-p3});
    CHECK(!(ByRatio{p3} < ByRatio{p1})); // not strictly worse
    CHECK(ByRatio{p2} < ByRatio{p1}); // strictly lower feerate

    // "empty" comparisons
    CHECK(!(ByRatio{p1} > ByRatio{empty})); // << will always result in false
    CHECK(!(ByRatio{p1} < ByRatio{empty}));
    CHECK(!(ByRatio{empty} > ByRatio{empty}));
    CHECK(!(ByRatio{empty} < ByRatio{empty}));

    // empty is always bigger than everything else
    CHECK(ByRatioNegSize{empty} > ByRatioNegSize{p1});
    CHECK(ByRatioNegSize{empty} > ByRatioNegSize{p2});
    CHECK(ByRatioNegSize{empty} > ByRatioNegSize{p3});
    CHECK(ByRatioNegSize{empty} >= ByRatioNegSize{p1});
    CHECK(ByRatioNegSize{empty} >= ByRatioNegSize{p2});
    CHECK(ByRatioNegSize{empty} >= ByRatioNegSize{p3});

    // check "max" values for comparison
    FeeFrac oversized_1{4611686000000, 4000000};
    FeeFrac oversized_2{184467440000000, 100000};

    CHECK(ByRatioNegSize{oversized_1} < ByRatioNegSize{oversized_2});
    CHECK(ByRatioNegSize{oversized_1} <= ByRatioNegSize{oversized_2});
    CHECK(ByRatio{oversized_1} < ByRatio{oversized_2});
    CHECK(ByRatioNegSize{oversized_1} != ByRatioNegSize{oversized_2});

    CHECK(oversized_1.EvaluateFeeDown(0) == 0);
    CHECK(oversized_1.EvaluateFeeDown(1) == 1152921);
    CHECK(oversized_1.EvaluateFeeDown(2) == 2305843);
    CHECK(oversized_1.EvaluateFeeDown(1548031267) == 1784758530396540);
    CHECK(oversized_1.EvaluateFeeUp(0) == 0);
    CHECK(oversized_1.EvaluateFeeUp(1) == 1152922);
    CHECK(oversized_1.EvaluateFeeUp(2) == 2305843);
    CHECK(oversized_1.EvaluateFeeUp(1548031267) == 1784758530396541);

    // Test cases on the threshold where FeeFrac::Evaluate start using Mul/Div.
    CHECK(FeeFrac(0x1ffffffff, 123456789).EvaluateFeeDown(98765432) == 6871947728);
    CHECK(FeeFrac(0x200000000, 123456789).EvaluateFeeDown(98765432) == 6871947729);
    CHECK(FeeFrac(0x200000001, 123456789).EvaluateFeeDown(98765432) == 6871947730);
    CHECK(FeeFrac(0x1ffffffff, 123456789).EvaluateFeeUp(98765432) == 6871947729);
    CHECK(FeeFrac(0x200000000, 123456789).EvaluateFeeUp(98765432) == 6871947730);
    CHECK(FeeFrac(0x200000001, 123456789).EvaluateFeeUp(98765432) == 6871947731);

    // Tests paths that use double arithmetic
    FeeFrac busted{(static_cast<int64_t>(INT32_MAX)) + 1, INT32_MAX};
    CHECK(!(ByRatioNegSize{busted} < ByRatioNegSize{busted}));

    FeeFrac max_fee{2100000000000000, INT32_MAX};
    CHECK(!(ByRatioNegSize{max_fee} < ByRatioNegSize{max_fee}));
    CHECK(!(ByRatioNegSize{max_fee} > ByRatioNegSize{max_fee}));
    CHECK(ByRatioNegSize{max_fee} <= ByRatioNegSize{max_fee});
    CHECK(ByRatioNegSize{max_fee} >= ByRatioNegSize{max_fee});

    CHECK(max_fee.EvaluateFeeDown(0) == 0);
    CHECK(max_fee.EvaluateFeeDown(1) == 977888);
    CHECK(max_fee.EvaluateFeeDown(2) == 1955777);
    CHECK(max_fee.EvaluateFeeDown(3) == 2933666);
    CHECK(max_fee.EvaluateFeeDown(1256796054) == 1229006664189047);
    CHECK(max_fee.EvaluateFeeDown(INT32_MAX) == 2100000000000000);
    CHECK(max_fee.EvaluateFeeUp(0) == 0);
    CHECK(max_fee.EvaluateFeeUp(1) == 977889);
    CHECK(max_fee.EvaluateFeeUp(2) == 1955778);
    CHECK(max_fee.EvaluateFeeUp(3) == 2933667);
    CHECK(max_fee.EvaluateFeeUp(1256796054) == 1229006664189048);
    CHECK(max_fee.EvaluateFeeUp(INT32_MAX) == 2100000000000000);

    FeeFrac max_fee2{1, 1};
    CHECK(ByRatioNegSize{max_fee} >= ByRatioNegSize{max_fee2});

    // Test for integer overflow issue (https://github.com/bitcoin/bitcoin/issues/32294)
    CHECK((FeeFrac{0x7ffffffdfffffffb, 0x7ffffffd}.EvaluateFeeDown(0x7fffffff)) == 0x7fffffffffffffff);
}

TEST_SUITE_END()
