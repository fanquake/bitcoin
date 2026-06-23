// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ipc/process.h>
#include <ipc/test/ipc_test.h>

#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <test/util/framework.h>

TEST_SUITE_BEGIN(ipc_tests)
FIXTURE_TEST_CASE(ipc_tests, BasicTestingSetup)
{
    IpcPipeTest();
    IpcSocketPairTest();
    IpcSocketTest(m_args.GetDataDirNet());
}

// Test address parsing.
FIXTURE_TEST_CASE(parse_address_test, BasicTestingSetup)
{
    std::unique_ptr<ipc::Process> process{ipc::MakeProcess()};
    fs::path datadir{"/var/empty/notexist"};
    auto check_notexist{[](const std::system_error& e) { return e.code() == std::errc::no_such_file_or_directory; }};
    auto check_address{[&](std::string address, std::string expect_address, std::string expect_error) {
        if (expect_error.empty()) {
            CHECK_EXCEPTION(process->connect(datadir, "test_bitcoin", address), std::system_error, check_notexist);
        } else {
            CHECK_EXCEPTION(process->connect(datadir, "test_bitcoin", address), std::invalid_argument, HasReason(expect_error));
        }
        CHECK(address == expect_address);
    }};
    check_address("unix", "unix:/var/empty/notexist/test_bitcoin.sock", "");
    check_address("unix:", "unix:/var/empty/notexist/test_bitcoin.sock", "");
    check_address("unix:path.sock", "unix:/var/empty/notexist/path.sock", "");
    check_address("unix:0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.sock",
                  "unix:/var/empty/notexist/0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.sock",
                  "Unix address path \"/var/empty/notexist/0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.sock\" exceeded maximum socket path length");
    check_address("invalid", "invalid", "Unrecognized address 'invalid'");
}

TEST_SUITE_END()
