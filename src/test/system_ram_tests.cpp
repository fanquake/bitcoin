// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <common/system.h>
#include <test/util/setup_common.h>

#include <test/util/framework.h>

#include <cstdint>
#include <optional>

TEST_SUITE_BEGIN(system_ram_tests)

TEST_CASE(total_ram)
{
    const auto total{GetTotalRAM()};
    if (!total) {
        WARN_MESSAGE(false, "skipping total_ram: total RAM unknown");
        return;
    }

    CHECK(*total >= 1000_MiB);
    CHECK(*total < 10'000_GiB); // ~10 TiB memory is unlikely
}

TEST_SUITE_END()
