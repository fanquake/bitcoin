// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <test/util/setup_common.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(sanity_tests)

FIXTURE_TEST_CASE(basic_sanity, BasicTestingSetup)
{
  CHECK(ECC_InitSanityCheck() == true, "secp256k1 sanity test");
}

TEST_SUITE_END()
