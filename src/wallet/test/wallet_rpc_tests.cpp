// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/request.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <wallet/rpc/util.h>

#include <test/util/framework.h>

#include <optional>
#include <string>

namespace wallet {
static std::string TestWalletName(const std::string& endpoint, std::optional<std::string> parameter = std::nullopt)
{
    JSONRPCRequest req;
    req.URI = endpoint;
    return EnsureUniqueWalletName(req, parameter);
}

TEST_SUITE_BEGIN(wallet_rpc_tests)

FIXTURE_TEST_CASE(ensure_unique_wallet_name, BasicTestingSetup)
{
    // EnsureUniqueWalletName should only return if exactly one unique wallet name is provided
    CHECK(TestWalletName("/wallet/foo") == "foo");
    CHECK(TestWalletName("/wallet/foo", "foo") == "foo");
    CHECK(TestWalletName("/", "foo") == "foo");
    CHECK(TestWalletName("/bar", "foo") == "foo");

    CHECK_THROWS_AS(TestWalletName("/"), UniValue);
    CHECK_THROWS_AS(TestWalletName("/foo"), UniValue);
    CHECK_THROWS_AS(TestWalletName("/wallet/foo", "bar"), UniValue);
    CHECK_THROWS_AS(TestWalletName("/wallet/foo", "foobar"), UniValue);
    CHECK_THROWS_AS(TestWalletName("/wallet/foobar", "foo"), UniValue);
}

TEST_SUITE_END()
} // namespace wallet
