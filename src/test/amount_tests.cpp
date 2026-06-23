// Copyright (c) 2016-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <policy/feerate.h>

#include <limits>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(amount_tests)

TEST_CASE(MoneyRangeTest)
{
    CHECK(MoneyRange(CAmount(-1)) == false);
    CHECK(MoneyRange(CAmount(0)) == true);
    CHECK(MoneyRange(CAmount(1)) == true);
    CHECK(MoneyRange(MAX_MONEY) == true);
    CHECK(MoneyRange(MAX_MONEY + CAmount(1)) == false);
}

TEST_CASE(GetFeeTest)
{
    CFeeRate feeRate, altFeeRate;

    feeRate = CFeeRate(0);
    // Must always return 0
    CHECK(feeRate.GetFee(0) == CAmount(0));
    CHECK(feeRate.GetFee(1e5) == CAmount(0));

    feeRate = CFeeRate(1000);
    // Must always just return the arg
    CHECK(feeRate.GetFee(0) == CAmount(0));
    CHECK(feeRate.GetFee(1) == CAmount(1));
    CHECK(feeRate.GetFee(121) == CAmount(121));
    CHECK(feeRate.GetFee(999) == CAmount(999));
    CHECK(feeRate.GetFee(1e3) == CAmount(1e3));
    CHECK(feeRate.GetFee(9e3) == CAmount(9e3));

    feeRate = CFeeRate(-1000);
    // Must always just return -1 * arg
    CHECK(feeRate.GetFee(0) == CAmount(0));
    CHECK(feeRate.GetFee(1) == CAmount(-1));
    CHECK(feeRate.GetFee(121) == CAmount(-121));
    CHECK(feeRate.GetFee(999) == CAmount(-999));
    CHECK(feeRate.GetFee(1e3) == CAmount(-1e3));
    CHECK(feeRate.GetFee(9e3) == CAmount(-9e3));

    feeRate = CFeeRate(123);
    // Rounds up the result, if not integer
    CHECK(feeRate.GetFee(0) == CAmount(0));
    CHECK(feeRate.GetFee(8) == CAmount(1)); // Special case: returns 1 instead of 0
    CHECK(feeRate.GetFee(9) == CAmount(2));
    CHECK(feeRate.GetFee(121) == CAmount(15));
    CHECK(feeRate.GetFee(122) == CAmount(16));
    CHECK(feeRate.GetFee(999) == CAmount(123));
    CHECK(feeRate.GetFee(1e3) == CAmount(123));
    CHECK(feeRate.GetFee(9e3) == CAmount(1107));

    feeRate = CFeeRate(-123);
    // Truncates the result, if not integer
    CHECK(feeRate.GetFee(0) == CAmount(0));
    CHECK(feeRate.GetFee(8) == CAmount(-1)); // Special case: returns -1 instead of 0
    CHECK(feeRate.GetFee(9) == CAmount(-1));

    // check alternate constructor
    feeRate = CFeeRate(1000);
    altFeeRate = CFeeRate(feeRate);
    CHECK(feeRate.GetFee(100) == altFeeRate.GetFee(100));

    // Check full constructor
    CHECK(CFeeRate(CAmount(-1), 0) == CFeeRate(0));
    CHECK(CFeeRate(CAmount(0), 0) == CFeeRate(0));
    CHECK(CFeeRate(CAmount(1), 0) == CFeeRate(0));
    CHECK(CFeeRate(CAmount(1), -1000) == CFeeRate(0));
    // default value
    CHECK(CFeeRate(CAmount(-1), 1000) == CFeeRate(-1));
    CHECK(CFeeRate(CAmount(0), 1000) == CFeeRate(0));
    CHECK(CFeeRate(CAmount(1), 1000) == CFeeRate(1));
    // Previously, precision was limited to three decimal digits
    // due to only supporting satoshis per kB, so CFeeRate(CAmount(1), 1001) was equal to CFeeRate(0)
    // Since #32750, higher precision is maintained.
    CHECK(CFeeRate(CAmount(1), 1001) > CFeeRate(0));
    CHECK(CFeeRate(CAmount(1), 1001) < CFeeRate(1));
    CHECK(CFeeRate(CAmount(2), 1001) > CFeeRate(1));
    CHECK(CFeeRate(CAmount(2), 1001) < CFeeRate(2));
    // some more integer checks
    CHECK(CFeeRate(CAmount(26), 789) > CFeeRate(32));
    CHECK(CFeeRate(CAmount(26), 789) < CFeeRate(33));
    CHECK(CFeeRate(CAmount(27), 789) > CFeeRate(34));
    CHECK(CFeeRate(CAmount(27), 789) < CFeeRate(35));
    // Maximum size in bytes, should not crash
    CFeeRate(MAX_MONEY, std::numeric_limits<int32_t>::max()).GetFeePerK();

    // check multiplication operator
    // check multiplying by zero
    feeRate = CFeeRate(1000);
    CHECK(0 * feeRate == CFeeRate(0));
    CHECK(feeRate * 0 == CFeeRate(0));
    // check multiplying by a positive integer
    CHECK(3 * feeRate == CFeeRate(3000));
    CHECK(feeRate * 3 == CFeeRate(3000));
    // check multiplying by a negative integer
    CHECK(-3 * feeRate == CFeeRate(-3000));
    CHECK(feeRate * -3 == CFeeRate(-3000));
    // check commutativity
    CHECK(2 * feeRate == feeRate * 2);
    // check with large numbers
    int largeNumber = 1000000;
    CHECK(largeNumber * feeRate == feeRate * largeNumber);
    // check boundary values
    int maxInt = std::numeric_limits<int>::max();
    feeRate = CFeeRate(maxInt);
    CHECK(feeRate * 2 == CFeeRate(static_cast<int64_t>(maxInt) * 2));
    CHECK(2 * feeRate == CFeeRate(static_cast<int64_t>(maxInt) * 2));
    // check with zero fee rate
    feeRate = CFeeRate(0);
    CHECK(feeRate * 5 == CFeeRate(0));
    CHECK(5 * feeRate == CFeeRate(0));
}

TEST_CASE(BinaryOperatorTest)
{
    CFeeRate a, b;
    a = CFeeRate(1);
    b = CFeeRate(2);
    CHECK(a < b);
    CHECK(b > a);
    CHECK(a == a);
    CHECK(a <= b);
    CHECK(a <= a);
    CHECK(b >= a);
    CHECK(b >= b);
    // a should be 0.00000002 BTC/kvB now
    a += a;
    CHECK(a == b);
}

TEST_CASE(ToStringTest)
{
    CFeeRate feeRate;
    feeRate = CFeeRate(1);
    CHECK(feeRate.ToString() == "0.00000001 BTC/kvB");
    CHECK(feeRate.ToString(FeeRateFormat::BTC_KVB) == "0.00000001 BTC/kvB");
    CHECK(feeRate.ToString(FeeRateFormat::SAT_VB) == "0.001 sat/vB");
}

TEST_SUITE_END()
