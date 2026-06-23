// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//

#include <node/timeoffsets.h>
#include <node/warnings.h>
#include <test/util/setup_common.h>

#include <test/util/framework.h>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

static void AddMulti(TimeOffsets& offsets, const std::vector<std::chrono::seconds>& to_add)
{
    for (auto offset : to_add) {
        offsets.Add(offset);
    }
}

TEST_SUITE_BEGIN(timeoffsets_tests)

FIXTURE_TEST_CASE(timeoffsets, BasicTestingSetup)
{
    node::Warnings warnings{};
    TimeOffsets offsets{warnings};
    CHECK(offsets.Median() == 0s);

    AddMulti(offsets, {{0s, -1s, -2s, -3s}});
    // median should be zero for < 5 offsets
    CHECK(offsets.Median() == 0s);

    offsets.Add(-4s);
    // we now have 5 offsets: [-4, -3, -2, -1, 0]
    CHECK(offsets.Median() == -2s);

    AddMulti(offsets, {4, 5s});
    // we now have 9 offsets: [-4, -3, -2, -1, 0, 5, 5, 5, 5]
    CHECK(offsets.Median() == 0s);

    AddMulti(offsets, {41, 10s});
    // the TimeOffsets is now at capacity with 50 offsets, oldest offsets is discarded for any additional offset
    CHECK(offsets.Median() == 10s);

    AddMulti(offsets, {25, 15s});
    // we now have 25 offsets of 10s followed by 25 offsets of 15s
    CHECK(offsets.Median() == 15s);
}

static bool IsWarningRaised(const std::vector<std::chrono::seconds>& check_offsets)
{
    node::Warnings warnings{};
    TimeOffsets offsets{warnings};
    AddMulti(offsets, check_offsets);
    return offsets.WarnIfOutOfSync();
}


FIXTURE_TEST_CASE(timeoffsets_warning, BasicTestingSetup)
{
    CHECK(IsWarningRaised({{-60min, -40min, -30min, 0min, 10min}}));
    CHECK(IsWarningRaised({5, 11min}));

    CHECK(!IsWarningRaised({4, 60min}));
    CHECK(!IsWarningRaised({100, 3min}));
}


TEST_SUITE_END()
