// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <common/settings.h>
#include <logging.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <test/util/framework.h>

using util::SplitString;

TEST_SUITE_BEGIN(getarg_tests)

void ResetArgs(ArgsManager& local_args, const std::string& strArg)
{
    std::vector<std::string> vecArg;
    if (strArg.size()) {
        vecArg = SplitString(strArg, ' ');
    }

    // Insert dummy executable name:
    vecArg.insert(vecArg.begin(), "testbitcoin");

    // Convert to char*:
    std::vector<const char*> vecChar;
    vecChar.reserve(vecArg.size());
    for (const std::string& s : vecArg)
        vecChar.push_back(s.c_str());

    std::string error;
    CHECK(local_args.ParseParameters(vecChar.size(), vecChar.data(), error));
}

void SetupArgs(ArgsManager& local_args, const std::vector<std::pair<std::string, unsigned int>>& args)
{
    for (const auto& arg : args) {
        local_args.AddArg(arg.first, "", arg.second, OptionsCategory::OPTIONS);
    }
}

// Test behavior of GetArg functions when string, integer, and boolean types
// are specified in the settings.json file. GetArg functions are convenience
// functions. The GetSetting method can always be used instead of GetArg
// methods to retrieve original values, and there's not always an objective
// answer to what GetArg behavior is best in every case. This test makes sure
// there's test coverage for whatever the current behavior is, so it's not
// broken or changed unintentionally.
FIXTURE_TEST_CASE(setting_args, BasicTestingSetup)
{
    ArgsManager args;
    SetupArgs(args, {{"-foo", ArgsManager::ALLOW_ANY}});

    auto set_foo = [&](const common::SettingsValue& value) {
      args.LockSettings([&](common::Settings& settings) {
        settings.rw_settings["foo"] = value;
      });
    };

    set_foo("str");
    CHECK(args.GetSetting("foo").write() == "\"str\"");
    CHECK(args.GetArg("foo", "default") == "str");
    CHECK(args.GetIntArg("foo", 100) == 0);
    CHECK(args.GetBoolArg("foo", true) == false);
    CHECK(args.GetBoolArg("foo", false) == false);

    set_foo("99");
    CHECK(args.GetSetting("foo").write() == "\"99\"");
    CHECK(args.GetArg("foo", "default") == "99");
    CHECK(args.GetIntArg("foo", 100) == 99);
    CHECK(args.GetBoolArg("foo", true) == true);
    CHECK(args.GetBoolArg("foo", false) == true);

    set_foo("3.25");
    CHECK(args.GetSetting("foo").write() == "\"3.25\"");
    CHECK(args.GetArg("foo", "default") == "3.25");
    CHECK(args.GetIntArg("foo", 100) == 3);
    CHECK(args.GetBoolArg("foo", true) == true);
    CHECK(args.GetBoolArg("foo", false) == true);

    set_foo("0");
    CHECK(args.GetSetting("foo").write() == "\"0\"");
    CHECK(args.GetArg("foo", "default") == "0");
    CHECK(args.GetIntArg("foo", 100) == 0);
    CHECK(args.GetBoolArg("foo", true) == false);
    CHECK(args.GetBoolArg("foo", false) == false);

    set_foo("");
    CHECK(args.GetSetting("foo").write() == "\"\"");
    CHECK(args.GetArg("foo", "default") == "");
    CHECK(args.GetIntArg("foo", 100) == 0);
    CHECK(args.GetBoolArg("foo", true) == true);
    CHECK(args.GetBoolArg("foo", false) == true);

    set_foo(99);
    CHECK(args.GetSetting("foo").write() == "99");
    CHECK(args.GetArg("foo", "default") == "99");
    CHECK(args.GetIntArg("foo", 100) == 99);
    CHECK_THROWS_AS(args.GetBoolArg("foo", true), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", false), std::runtime_error);

    set_foo(3.25);
    CHECK(args.GetSetting("foo").write() == "3.25");
    CHECK(args.GetArg("foo", "default") == "3.25");
    CHECK_THROWS_AS(args.GetIntArg("foo", 100), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", true), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", false), std::runtime_error);

    set_foo(0);
    CHECK(args.GetSetting("foo").write() == "0");
    CHECK(args.GetArg("foo", "default") == "0");
    CHECK(args.GetIntArg("foo", 100) == 0);
    CHECK_THROWS_AS(args.GetBoolArg("foo", true), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", false), std::runtime_error);

    set_foo(true);
    CHECK(args.GetSetting("foo").write() == "true");
    CHECK(args.GetArg("foo", "default") == "1");
    CHECK(args.GetIntArg("foo", 100) == 1);
    CHECK(args.GetBoolArg("foo", true) == true);
    CHECK(args.GetBoolArg("foo", false) == true);

    set_foo(false);
    CHECK(args.GetSetting("foo").write() == "false");
    CHECK(args.GetArg("foo", "default") == "0");
    CHECK(args.GetIntArg("foo", 100) == 0);
    CHECK(args.GetBoolArg("foo", true) == false);
    CHECK(args.GetBoolArg("foo", false) == false);

    set_foo(UniValue::VOBJ);
    CHECK(args.GetSetting("foo").write() == "{}");
    CHECK_THROWS_AS(args.GetArg("foo", "default"), std::runtime_error);
    CHECK_THROWS_AS(args.GetIntArg("foo", 100), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", true), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", false), std::runtime_error);

    set_foo(UniValue::VARR);
    CHECK(args.GetSetting("foo").write() == "[]");
    CHECK_THROWS_AS(args.GetArg("foo", "default"), std::runtime_error);
    CHECK_THROWS_AS(args.GetIntArg("foo", 100), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", true), std::runtime_error);
    CHECK_THROWS_AS(args.GetBoolArg("foo", false), std::runtime_error);

    set_foo(UniValue::VNULL);
    CHECK(args.GetSetting("foo").write() == "null");
    CHECK(args.GetArg("foo", "default") == "default");
    CHECK(args.GetIntArg("foo", 100) == 100);
    CHECK(args.GetBoolArg("foo", true) == true);
    CHECK(args.GetBoolArg("foo", false) == false);
}

FIXTURE_TEST_CASE(boolarg, BasicTestingSetup)
{
    ArgsManager local_args;

    const auto foo = std::make_pair("-foo", ArgsManager::ALLOW_ANY);
    SetupArgs(local_args, {foo});
    ResetArgs(local_args, "-foo");
    CHECK(local_args.GetBoolArg("-foo", false));
    CHECK(local_args.GetBoolArg("-foo", true));

    CHECK(!local_args.GetBoolArg("-fo", false));
    CHECK(local_args.GetBoolArg("-fo", true));

    CHECK(!local_args.GetBoolArg("-fooo", false));
    CHECK(local_args.GetBoolArg("-fooo", true));

    ResetArgs(local_args, "-foo=0");
    CHECK(!local_args.GetBoolArg("-foo", false));
    CHECK(!local_args.GetBoolArg("-foo", true));

    ResetArgs(local_args, "-foo=1");
    CHECK(local_args.GetBoolArg("-foo", false));
    CHECK(local_args.GetBoolArg("-foo", true));

    // New 0.6 feature: auto-map -nosomething to !-something:
    ResetArgs(local_args, "-nofoo");
    CHECK(!local_args.GetBoolArg("-foo", false));
    CHECK(!local_args.GetBoolArg("-foo", true));

    ResetArgs(local_args, "-nofoo=1");
    CHECK(!local_args.GetBoolArg("-foo", false));
    CHECK(!local_args.GetBoolArg("-foo", true));

    ResetArgs(local_args, "-foo -nofoo"); // -nofoo should win
    CHECK(!local_args.GetBoolArg("-foo", false));
    CHECK(!local_args.GetBoolArg("-foo", true));

    ResetArgs(local_args, "-foo=1 -nofoo=1"); // -nofoo should win
    CHECK(!local_args.GetBoolArg("-foo", false));
    CHECK(!local_args.GetBoolArg("-foo", true));

    ResetArgs(local_args, "-foo=0 -nofoo=0"); // -nofoo=0 should win
    CHECK(local_args.GetBoolArg("-foo", false));
    CHECK(local_args.GetBoolArg("-foo", true));

    // New 0.6 feature: treat -- same as -:
    ResetArgs(local_args, "--foo=1");
    CHECK(local_args.GetBoolArg("-foo", false));
    CHECK(local_args.GetBoolArg("-foo", true));

    ResetArgs(local_args, "--nofoo=1");
    CHECK(!local_args.GetBoolArg("-foo", false));
    CHECK(!local_args.GetBoolArg("-foo", true));
}

FIXTURE_TEST_CASE(stringarg, BasicTestingSetup)
{
    ArgsManager local_args;

    const auto foo = std::make_pair("-foo", ArgsManager::ALLOW_ANY);
    const auto bar = std::make_pair("-bar", ArgsManager::ALLOW_ANY);
    SetupArgs(local_args, {foo, bar});
    ResetArgs(local_args, "");
    CHECK(local_args.GetArg("-foo", "") == "");
    CHECK(local_args.GetArg("-foo", "eleven") == "eleven");

    ResetArgs(local_args, "-foo -bar");
    CHECK(local_args.GetArg("-foo", "") == "");
    CHECK(local_args.GetArg("-foo", "eleven") == "");

    ResetArgs(local_args, "-foo=");
    CHECK(local_args.GetArg("-foo", "") == "");
    CHECK(local_args.GetArg("-foo", "eleven") == "");

    ResetArgs(local_args, "-foo=11");
    CHECK(local_args.GetArg("-foo", "") == "11");
    CHECK(local_args.GetArg("-foo", "eleven") == "11");

    ResetArgs(local_args, "-foo=eleven");
    CHECK(local_args.GetArg("-foo", "") == "eleven");
    CHECK(local_args.GetArg("-foo", "eleven") == "eleven");
}

FIXTURE_TEST_CASE(intarg, BasicTestingSetup)
{
    ArgsManager local_args;

    const auto foo = std::make_pair("-foo", ArgsManager::ALLOW_ANY);
    const auto bar = std::make_pair("-bar", ArgsManager::ALLOW_ANY);
    SetupArgs(local_args, {foo, bar});

    ResetArgs(local_args, "");
    CHECK(!local_args.GetArg<int64_t>("-foo").has_value());
    CHECK(!local_args.GetArg<uint8_t>("-bar").has_value());
    CHECK(local_args.GetIntArg("-foo", 11) == 11);
    CHECK(local_args.GetIntArg("-foo", 0) == 0);
    CHECK(local_args.GetArg("-bar", uint8_t{222}) == 222);
    CHECK(local_args.GetArg("-bar", uint8_t{0}) == 0);

    ResetArgs(local_args, "-foo -bar");
    CHECK(local_args.GetArg<int64_t>("-foo") == 0);
    CHECK(local_args.GetArg<uint8_t>("-bar") == 0);
    CHECK(local_args.GetIntArg("-foo", 11) == 0);
    CHECK(local_args.GetArg("-bar", uint8_t{222}) == 0);

    // Check under-/overflow behavior.
    ResetArgs(local_args, "-foo=-9223372036854775809 -bar=9223372036854775808");
    CHECK(local_args.GetArg<int64_t>("-foo") == std::numeric_limits<int64_t>::min());
    CHECK(local_args.GetArg<uint8_t>("-bar") == std::numeric_limits<uint8_t>::max());
    CHECK(local_args.GetIntArg("-foo", 0) == std::numeric_limits<int64_t>::min());
    CHECK(local_args.GetIntArg("-bar", 0) == std::numeric_limits<int64_t>::max());
    CHECK(local_args.GetArg("-foo", uint8_t{0}) == std::numeric_limits<uint8_t>::min());
    CHECK(local_args.GetArg("-bar", uint8_t{0}) == std::numeric_limits<uint8_t>::max());

    ResetArgs(local_args, "-foo=11 -bar=12");
    CHECK(local_args.GetArg<int64_t>("-foo") == 11);
    CHECK(local_args.GetArg<uint8_t>("-bar") == 12);
    CHECK(local_args.GetIntArg("-foo", 0) == 11);
    CHECK(local_args.GetArg("-bar", uint8_t{11}) == 12);

    ResetArgs(local_args, "-foo=NaN -bar=NotANumber");
    CHECK(local_args.GetArg<int64_t>("-foo") == 0);
    CHECK(local_args.GetArg<uint8_t>("-bar") == 0);
    CHECK(local_args.GetIntArg("-foo", 1) == 0);
    CHECK(local_args.GetArg("-bar", uint8_t{11}) == 0);
}

FIXTURE_TEST_CASE(patharg, BasicTestingSetup)
{
    ArgsManager local_args;

    const auto dir = std::make_pair("-dir", ArgsManager::ALLOW_ANY);
    SetupArgs(local_args, {dir});
    ResetArgs(local_args, "");
    CHECK(local_args.GetPathArg("-dir") == fs::path{});

    const fs::path root_path{"/"};
    ResetArgs(local_args, "-dir=/");
    CHECK(local_args.GetPathArg("-dir") == root_path);

    ResetArgs(local_args, "-dir=/.");
    CHECK(local_args.GetPathArg("-dir") == root_path);

    ResetArgs(local_args, "-dir=/./");
    CHECK(local_args.GetPathArg("-dir") == root_path);

    ResetArgs(local_args, "-dir=/.//");
    CHECK(local_args.GetPathArg("-dir") == root_path);

#ifdef WIN32
    const fs::path win_root_path{"C:\\"};
    ResetArgs(local_args, "-dir=C:\\");
    CHECK(local_args.GetPathArg("-dir") == win_root_path);

    ResetArgs(local_args, "-dir=C:/");
    CHECK(local_args.GetPathArg("-dir") == win_root_path);

    ResetArgs(local_args, "-dir=C:\\\\");
    CHECK(local_args.GetPathArg("-dir") == win_root_path);

    ResetArgs(local_args, "-dir=C:\\.");
    CHECK(local_args.GetPathArg("-dir") == win_root_path);

    ResetArgs(local_args, "-dir=C:\\.\\");
    CHECK(local_args.GetPathArg("-dir") == win_root_path);

    ResetArgs(local_args, "-dir=C:\\.\\\\");
    CHECK(local_args.GetPathArg("-dir") == win_root_path);
#endif

    const fs::path absolute_path{"/home/user/.bitcoin"};
    ResetArgs(local_args, "-dir=/home/user/.bitcoin");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    ResetArgs(local_args, "-dir=/root/../home/user/.bitcoin");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    ResetArgs(local_args, "-dir=/home/./user/.bitcoin");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    ResetArgs(local_args, "-dir=/home/user/.bitcoin/");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    ResetArgs(local_args, "-dir=/home/user/.bitcoin//");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    ResetArgs(local_args, "-dir=/home/user/.bitcoin/.");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    ResetArgs(local_args, "-dir=/home/user/.bitcoin/./");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    ResetArgs(local_args, "-dir=/home/user/.bitcoin/.//");
    CHECK(local_args.GetPathArg("-dir") == absolute_path);

    const fs::path relative_path{"user/.bitcoin"};
    ResetArgs(local_args, "-dir=user/.bitcoin");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    ResetArgs(local_args, "-dir=somewhere/../user/.bitcoin");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    ResetArgs(local_args, "-dir=user/./.bitcoin");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    ResetArgs(local_args, "-dir=user/.bitcoin/");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    ResetArgs(local_args, "-dir=user/.bitcoin//");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    ResetArgs(local_args, "-dir=user/.bitcoin/.");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    ResetArgs(local_args, "-dir=user/.bitcoin/./");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    ResetArgs(local_args, "-dir=user/.bitcoin/.//");
    CHECK(local_args.GetPathArg("-dir") == relative_path);

    // Check negated and default argument handling. Specifying an empty argument
    // is the same as not specifying the argument. This is convenient for
    // scripting so later command line arguments can override earlier command
    // line arguments or bitcoin.conf values. Currently the -dir= case cannot be
    // distinguished from -dir case with no assignment, but #16545 would add the
    // ability to distinguish these in the future (and treat the no-assign case
    // like an imperative command or an error).
    ResetArgs(local_args, "");
    CHECK(local_args.GetPathArg("-dir", "default") == fs::path{"default"});
    ResetArgs(local_args, "-dir=override");
    CHECK(local_args.GetPathArg("-dir", "default") == fs::path{"override"});
    ResetArgs(local_args, "-dir=");
    CHECK(local_args.GetPathArg("-dir", "default") == fs::path{"default"});
    ResetArgs(local_args, "-dir");
    CHECK(local_args.GetPathArg("-dir", "default") == fs::path{"default"});
    ResetArgs(local_args, "-nodir");
    CHECK(local_args.GetPathArg("-dir", "default") == fs::path{""});
}

FIXTURE_TEST_CASE(doubledash, BasicTestingSetup)
{
    ArgsManager local_args;

    const auto foo = std::make_pair("-foo", ArgsManager::ALLOW_ANY);
    const auto bar = std::make_pair("-bar", ArgsManager::ALLOW_ANY);
    SetupArgs(local_args, {foo, bar});
    ResetArgs(local_args, "--foo");
    CHECK(local_args.GetBoolArg("-foo", false) == true);

    ResetArgs(local_args, "--foo=verbose --bar=1");
    CHECK(local_args.GetArg("-foo", "") == "verbose");
    CHECK(local_args.GetIntArg("-bar", 0) == 1);
}

FIXTURE_TEST_CASE(boolargno, BasicTestingSetup)
{
    ArgsManager local_args;

    const auto foo = std::make_pair("-foo", ArgsManager::ALLOW_ANY);
    const auto bar = std::make_pair("-bar", ArgsManager::ALLOW_ANY);
    SetupArgs(local_args, {foo, bar});
    ResetArgs(local_args, "-nofoo");
    CHECK(!local_args.GetBoolArg("-foo", true));
    CHECK(!local_args.GetBoolArg("-foo", false));

    ResetArgs(local_args, "-nofoo=1");
    CHECK(!local_args.GetBoolArg("-foo", true));
    CHECK(!local_args.GetBoolArg("-foo", false));

    ResetArgs(local_args, "-nofoo=0");
    CHECK(local_args.GetBoolArg("-foo", true));
    CHECK(local_args.GetBoolArg("-foo", false));

    ResetArgs(local_args, "-foo --nofoo"); // --nofoo should win
    CHECK(!local_args.GetBoolArg("-foo", true));
    CHECK(!local_args.GetBoolArg("-foo", false));

    ResetArgs(local_args, "-nofoo -foo"); // foo always wins:
    CHECK(local_args.GetBoolArg("-foo", true));
    CHECK(local_args.GetBoolArg("-foo", false));
}

FIXTURE_TEST_CASE(logargs, BasicTestingSetup)
{
    ArgsManager local_args;

    const auto okaylog_bool = std::make_pair("-okaylog-bool", ArgsManager::ALLOW_ANY);
    const auto okaylog_negbool = std::make_pair("-okaylog-negbool", ArgsManager::ALLOW_ANY);
    const auto okaylog = std::make_pair("-okaylog", ArgsManager::ALLOW_ANY);
    const auto dontlog = std::make_pair("-dontlog", ArgsManager::ALLOW_ANY | ArgsManager::SENSITIVE);
    SetupArgs(local_args, {okaylog_bool, okaylog_negbool, okaylog, dontlog});
    ResetArgs(local_args, "-okaylog-bool -nookaylog-negbool -okaylog=public -dontlog=private42");

    // Everything logged to debug.log will also append to str
    std::string str;
    auto print_connection = LogInstance().PushBackCallback(
        [&str](const std::string& s) {
            str += s;
        });

    // Log the arguments
    local_args.LogArgs();

    LogInstance().DeleteCallback(print_connection);
    // Check that what should appear does, and what shouldn't doesn't.
    CHECK(str.find("Command-line arg: okaylog-bool=\"\"") != std::string::npos);
    CHECK(str.find("Command-line arg: okaylog-negbool=false") != std::string::npos);
    CHECK(str.find("Command-line arg: okaylog=\"public\"") != std::string::npos);
    CHECK(str.find("dontlog=****") != std::string::npos);
    CHECK(str.find("private42") == std::string::npos);
}

TEST_SUITE_END()
