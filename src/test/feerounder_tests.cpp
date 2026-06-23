// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <policy/fees/block_policy_estimator.h>

#include <test/util/framework.h>

#include <set>

TEST_SUITE_BEGIN(feerounder_tests)

TEST_CASE(FeeRounder)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    FeeFilterRounder fee_rounder{CFeeRate{1000}, rng};

    // check that 1000 rounds to 974 or 1071
    std::set<CAmount> results;
    while (results.size() < 2) {
        results.emplace(fee_rounder.round(1000));
    }
    CHECK(*results.begin() == 974);
    CHECK(*++results.begin() == 1071);

    // check that negative amounts rounds to 0
    CHECK(fee_rounder.round(-0) == 0);
    CHECK(fee_rounder.round(-1) == 0);

    // check that MAX_MONEY rounds to 9170997
    CHECK(fee_rounder.round(MAX_MONEY) == 9170997);
}

TEST_SUITE_END()
