// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/transaction.h>

#include <test/util/common.h>
#include <wallet/test/wallet_test_fixture.h>

#include <test/util/framework.h>

namespace wallet {
TEST_SUITE_BEGIN(wallet_transaction_tests)

FIXTURE_TEST_CASE(roundtrip, WalletTestingSetup)
{
    for (uint8_t hash = 0; hash < 5; ++hash) {
        for (int index = -2; index < 3; ++index) {
            TxState state = TxStateInterpretSerialized(TxStateUnrecognized{uint256{hash}, index});
            CHECK(TxStateSerializedBlockHash(state) == uint256{hash});
            CHECK(TxStateSerializedIndex(state) == index);
        }
    }
}

TEST_SUITE_END()
} // namespace wallet
