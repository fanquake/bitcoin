// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/framework.h>

#include <common/args.h>
#include <noui.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <wallet/test/init_test_fixture.h>

namespace wallet {
TEST_SUITE_BEGIN(init_tests)

FIXTURE_TEST_CASE(walletinit_verify_walletdir_default, InitWalletDirTestingSetup)
{
    SetWalletDir(m_walletdir_path_cases["default"]);
    bool result = m_wallet_loader->verify();
    CHECK(result == true);
    fs::path walletdir = m_args.GetPathArg("-walletdir");
    fs::path expected_path = fs::canonical(m_walletdir_path_cases["default"]);
    CHECK(walletdir == expected_path);
}

FIXTURE_TEST_CASE(walletinit_verify_walletdir_custom, InitWalletDirTestingSetup)
{
    SetWalletDir(m_walletdir_path_cases["custom"]);
    bool result = m_wallet_loader->verify();
    CHECK(result == true);
    fs::path walletdir = m_args.GetPathArg("-walletdir");
    fs::path expected_path = fs::canonical(m_walletdir_path_cases["custom"]);
    CHECK(walletdir == expected_path);
}

FIXTURE_TEST_CASE(walletinit_verify_walletdir_does_not_exist, InitWalletDirTestingSetup)
{
    SetWalletDir(m_walletdir_path_cases["nonexistent"]);
    {
        ASSERT_DEBUG_LOG("does not exist");
        bool result = m_wallet_loader->verify();
        CHECK(result == false);
    }
}

FIXTURE_TEST_CASE(walletinit_verify_walletdir_is_not_directory, InitWalletDirTestingSetup)
{
    SetWalletDir(m_walletdir_path_cases["file"]);
    {
        ASSERT_DEBUG_LOG("is not a directory");
        bool result = m_wallet_loader->verify();
        CHECK(result == false);
    }
}

FIXTURE_TEST_CASE(walletinit_verify_walletdir_is_not_relative, InitWalletDirTestingSetup)
{
    SetWalletDir(m_walletdir_path_cases["relative"]);
    {
        ASSERT_DEBUG_LOG("is a relative path");
        bool result = m_wallet_loader->verify();
        CHECK(result == false);
    }
}

FIXTURE_TEST_CASE(walletinit_verify_walletdir_no_trailing, InitWalletDirTestingSetup)
{
    SetWalletDir(m_walletdir_path_cases["trailing"]);
    bool result = m_wallet_loader->verify();
    CHECK(result == true);
    fs::path walletdir = m_args.GetPathArg("-walletdir");
    fs::path expected_path = fs::canonical(m_walletdir_path_cases["default"]);
    CHECK(walletdir == expected_path);
}

FIXTURE_TEST_CASE(walletinit_verify_walletdir_no_trailing2, InitWalletDirTestingSetup)
{
    SetWalletDir(m_walletdir_path_cases["trailing2"]);
    bool result = m_wallet_loader->verify();
    CHECK(result == true);
    fs::path walletdir = m_args.GetPathArg("-walletdir");
    fs::path expected_path = fs::canonical(m_walletdir_path_cases["default"]);
    CHECK(walletdir == expected_path);
}

TEST_SUITE_END()
} // namespace wallet
