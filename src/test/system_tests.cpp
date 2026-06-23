// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <common/run_command.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/string.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

#ifdef ENABLE_EXTERNAL_SIGNER
#include <util/subprocess.h>
#endif // ENABLE_EXTERNAL_SIGNER

#include <test/util/framework.h>

#include <string>

namespace {
// When set in the environment, test_bitcoin acts as a mock subprocess for the
// run_command test below instead of running unit tests. Dispatched from a
// static initializer so the process exits before the test framework starts.
constexpr const char* MOCK_PROCESS_ENV = "BITCOIN_TEST_MOCK_PROCESS";

struct MockProcessDispatcher {
    MockProcessDispatcher()
    {
        const char* name = std::getenv(MOCK_PROCESS_ENV);
        if (!name) return;
        const std::string_view n{name};
        if (n == "valid_json") {
            std::cout << R"({"success": true})" << std::endl;
            std::_Exit(EXIT_SUCCESS);
        }
        if (n == "nonzeroexit_nooutput") {
            std::_Exit(EXIT_FAILURE);
        }
        if (n == "nonzeroexit_stderroutput") {
            std::cerr << "err" << std::endl;
            std::_Exit(EXIT_FAILURE);
        }
        if (n == "invalid_json") {
            std::cout << "{" << std::endl;
            std::_Exit(EXIT_SUCCESS);
        }
        if (n == "pass_stdin_to_stdout") {
            std::string s;
            std::getline(std::cin, s);
            std::cout << s << std::endl;
            std::_Exit(EXIT_SUCCESS);
        }
        std::cerr << "Unknown mock process: " << n << std::endl;
        std::_Exit(EXIT_FAILURE);
    }
};
const MockProcessDispatcher g_mock_dispatcher;
} // namespace

TEST_SUITE_BEGIN(system_tests)

#ifdef ENABLE_EXTERNAL_SIGNER

static std::vector<std::string> mock_executable(const std::string& name)
{
#if defined(WIN32)
    _putenv_s(MOCK_PROCESS_ENV, name.c_str());
#else
    setenv(MOCK_PROCESS_ENV, name.c_str(), /*overwrite=*/1);
#endif
    return {framework::executable_path()};
}

FIXTURE_TEST_CASE(run_command, BasicTestingSetup)
{
    {
        const UniValue result = RunCommandParseJSON({});
        CHECK(result.isNull());
    }
    {
        const UniValue result = RunCommandParseJSON(mock_executable("valid_json"));
        CHECK(result.isObject());
        const UniValue& success = result.find_value("success");
        CHECK(!success.isNull());
        CHECK(success.get_bool() == true);
    }
    {
        // An invalid command is handled by cpp-subprocess
#ifdef WIN32
        const std::string expected{"CreateProcess failed: "};
#else
        const std::string expected{"execve failed: "};
#endif
        CHECK_EXCEPTION(RunCommandParseJSON({"invalid_command"}), subprocess::CalledProcessError, HasReason(expected));
    }
    {
        // Return non-zero exit code, no output to stderr
        const std::vector<std::string> command = mock_executable("nonzeroexit_nooutput");
        CHECK_EXCEPTION(RunCommandParseJSON(command), std::runtime_error, [&](const std::runtime_error& e) {
            const std::string what{e.what()};
            CHECK(what.find(strprintf("RunCommandParseJSON error: process(%s) returned %d: \n", util::Join(command, " "), EXIT_FAILURE)) != std::string::npos);
            return true;
        });
    }
    {
        // Return non-zero exit code, with error message for stderr
        const std::vector<std::string> command = mock_executable("nonzeroexit_stderroutput");
        const std::string expected{"err"};
        CHECK_EXCEPTION(RunCommandParseJSON(command), std::runtime_error, [&](const std::runtime_error& e) {
            const std::string what(e.what());
            CHECK(what.find(strprintf("RunCommandParseJSON error: process(%s) returned %s: %s", util::Join(command, " "), EXIT_FAILURE, "err")) != std::string::npos);
            CHECK(what.find(expected) != std::string::npos);
            return true;
        });
    }
    {
        // Unable to parse JSON
        CHECK_EXCEPTION(RunCommandParseJSON(mock_executable("invalid_json")), std::runtime_error, HasReason("Unable to parse JSON: {"));
    }
    {
        // Test stdin
        const UniValue result = RunCommandParseJSON(mock_executable("pass_stdin_to_stdout"), "{\"success\": true}");
        CHECK(result.isObject());
        const UniValue& success = result.find_value("success");
        CHECK(!success.isNull());
        CHECK(success.get_bool() == true);
    }
}
#endif // ENABLE_EXTERNAL_SIGNER

TEST_SUITE_END()
