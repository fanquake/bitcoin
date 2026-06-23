// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/settings.h>

#include <test/util/setup_common.h>
#include <test/util/str.h>

#include <test/util/framework.h>

#include <common/args.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

using util::ToString;

inline bool operator==(const common::SettingsValue& a, const common::SettingsValue& b)
{
    return a.write() == b.write();
}

inline std::ostream& operator<<(std::ostream& os, const common::SettingsValue& value)
{
    os << value.write();
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const std::pair<std::string, common::SettingsValue>& kv)
{
    common::SettingsValue out(common::SettingsValue::VOBJ);
    out.pushKVEnd(kv.first, kv.second);
    os << out.write();
    return os;
}

inline void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream file;
    file.open(path.std_path());
    file << text;
}

TEST_SUITE_BEGIN(settings_tests)

FIXTURE_TEST_CASE(ReadWrite, BasicTestingSetup)
{
    fs::path path = m_args.GetDataDirBase() / "settings.json";

    WriteText(path, R"({
        "string": "string",
        "num": 5,
        "bool": true,
        "null": null
    })");

    std::map<std::string, common::SettingsValue> expected{
        {"string", "string"},
        {"num", 5},
        {"bool", true},
        {"null", {}},
    };

    // Check file read.
    std::map<std::string, common::SettingsValue> values;
    std::vector<std::string> errors;
    CHECK(common::ReadSettings(path, values, errors));
    CHECK_EQUAL_RANGES(values, expected);
    CHECK(errors.empty());

    // Check no errors if file doesn't exist.
    fs::remove(path);
    CHECK(common::ReadSettings(path, values, errors));
    CHECK(values.empty());
    CHECK(errors.empty());

    // Check duplicate keys not allowed and that values returns empty if a duplicate is found.
    WriteText(path, R"({
        "dupe": "string",
        "dupe": "dupe"
    })");
    CHECK(!common::ReadSettings(path, values, errors));
    std::vector<std::string> dup_keys = {strprintf("Found duplicate key dupe in settings file %s", fs::PathToString(path))};
    CHECK_EQUAL_RANGES(errors, dup_keys);
    CHECK(values.empty());

    // Check non-kv json files not allowed
    WriteText(path, R"("non-kv")");
    CHECK(!common::ReadSettings(path, values, errors));
    std::vector<std::string> non_kv = {strprintf("Found non-object value \"non-kv\" in settings file %s", fs::PathToString(path))};
    CHECK_EQUAL_RANGES(errors, non_kv);

    // Check invalid json not allowed
    WriteText(path, R"(invalid json)");
    CHECK(!common::ReadSettings(path, values, errors));
    std::vector<std::string> fail_parse = {strprintf("Settings file %s does not contain valid JSON. This may be caused by a crash, power loss, full disk, or storage error, "
                                                     "and can be fixed by removing the file, which will reset settings to default values.",
                                                     fs::PathToString(path))};
    CHECK_EQUAL_RANGES(errors, fail_parse);
}

//! Check settings struct contents against expected json strings.
static void CheckValues(const common::Settings& settings, const std::string& single_val, const std::string& list_val)
{
    common::SettingsValue single_value = GetSetting(settings, "section", "name", false, false, false);
    common::SettingsValue list_value(common::SettingsValue::VARR);
    for (const auto& item : GetSettingsList(settings, "section", "name", false)) {
        list_value.push_back(item);
    }
    CHECK(single_value.write() == single_val);
    CHECK(list_value.write() == list_val);
};

// Simple settings merge test case.
FIXTURE_TEST_CASE(Simple, BasicTestingSetup)
{
    common::Settings settings;
    settings.command_line_options["name"].emplace_back("val1");
    settings.command_line_options["name"].emplace_back("val2");
    settings.ro_config["section"]["name"].emplace_back(2);

    // The last given arg takes precedence when specified via commandline.
    CheckValues(settings, R"("val2")", R"(["val1","val2",2])");

    common::Settings settings2;
    settings2.ro_config["section"]["name"].emplace_back("val2");
    settings2.ro_config["section"]["name"].emplace_back("val3");

    // The first given arg takes precedence when specified via config file.
    CheckValues(settings2, R"("val2")", R"(["val2","val3"])");
}

// Confirm that a high priority setting overrides a lower priority setting even
// if the high priority setting is null. This behavior is useful for a high
// priority setting source to be able to effectively reset any setting back to
// its default value.
FIXTURE_TEST_CASE(NullOverride, BasicTestingSetup)
{
    common::Settings settings;
    settings.command_line_options["name"].emplace_back("value");
    CHECK(std::string{R"("value")"} == GetSetting(settings, "section", "name", false, false, false).write());
    settings.forced_settings["name"] = {};
    CHECK(std::string{R"(null)"} == GetSetting(settings, "section", "name", false, false, false).write());
}

// Test different ways settings can be merged, and verify results. This test can
// be used to confirm that updates to settings code don't change behavior
// unintentionally.
struct MergeTestingSetup : public BasicTestingSetup {
    //! Max number of actions to sequence together. Can decrease this when
    //! debugging to make test results easier to understand.
    static constexpr int MAX_ACTIONS = 3;

    enum Action { END, SET, NEGATE, SECTION_SET, SECTION_NEGATE };
    using ActionList = Action[MAX_ACTIONS];

    //! Enumerate all possible test configurations.
    template <typename Fn>
    void ForEachMergeSetup(Fn&& fn)
    {
        ActionList arg_actions = {};
        // command_line_options do not have sections. Only iterate over SET and NEGATE
        ForEachNoDup(arg_actions, SET, NEGATE, [&]{
            ActionList conf_actions = {};
            ForEachNoDup(conf_actions, SET, SECTION_NEGATE, [&]{
                for (bool force_set : {false, true}) {
                    for (bool ignore_default_section_config : {false, true}) {
                        fn(arg_actions, conf_actions, force_set, ignore_default_section_config);
                    }
                }
            });
        });
    }
};

// Regression test covering different ways config settings can be merged. The
// test parses and merges settings, representing the results as strings that get
// compared against an expected hash. To debug, the result strings can be dumped
// to a file (see comments below).
FIXTURE_TEST_CASE(Merge, MergeTestingSetup)
{
    CHash256 out_sha;
    FILE* out_file = nullptr;
    if (const char* out_path = getenv("SETTINGS_MERGE_TEST_OUT")) {
        out_file = fsbridge::fopen(out_path, "w");
        if (!out_file) throw std::system_error(errno, std::generic_category(), "fopen failed");
    }

    const std::string& network = ChainTypeToString(ChainType::MAIN);
    ForEachMergeSetup([&](const ActionList& arg_actions, const ActionList& conf_actions, bool force_set,
                          bool ignore_default_section_config) {
        std::string desc;
        int value_suffix = 0;
        common::Settings settings;

        const std::string& name = ignore_default_section_config ? "wallet" : "server";
        auto push_values = [&](Action action, const char* value_prefix, const std::string& name_prefix,
                               std::vector<common::SettingsValue>& dest) {
            if (action == SET || action == SECTION_SET) {
                for (int i = 0; i < 2; ++i) {
                    dest.emplace_back(value_prefix + ToString(++value_suffix));
                    desc += " " + name_prefix + name + "=" + dest.back().get_str();
                }
            } else if (action == NEGATE || action == SECTION_NEGATE) {
                dest.emplace_back(false);
                desc += " " + name_prefix + "no" + name;
            }
        };

        if (force_set) {
            settings.forced_settings[name] = "forced";
            desc += " " + name + "=forced";
        }
        for (Action arg_action : arg_actions) {
            push_values(arg_action, "a", "-", settings.command_line_options[name]);
        }
        for (Action conf_action : conf_actions) {
            bool use_section = conf_action == SECTION_SET || conf_action == SECTION_NEGATE;
            push_values(conf_action, "c", use_section ? network + "." : "",
                settings.ro_config[use_section ? network : ""][name]);
        }

        desc += " || ";
        desc += GetSetting(settings, network, name, ignore_default_section_config, /*ignore_nonpersistent=*/false, /*get_chain_type=*/false).write();
        desc += " |";
        for (const auto& s : GetSettingsList(settings, network, name, ignore_default_section_config)) {
            desc += " ";
            desc += s.write();
        }
        desc += " |";
        if (OnlyHasDefaultSectionSetting(settings, network, name)) desc += " ignored";
        desc += "\n";

        out_sha.Write(MakeUCharSpan(desc));
        if (out_file) {
            REQUIRE(fwrite(desc.data(), 1, desc.size(), out_file) == desc.size());
        }
    });

    if (out_file) {
        if (fclose(out_file)) throw std::system_error(errno, std::generic_category(), "fclose failed");
        out_file = nullptr;
    }

    unsigned char out_sha_bytes[CSHA256::OUTPUT_SIZE];
    out_sha.Finalize(out_sha_bytes);
    std::string out_sha_hex = HexStr(out_sha_bytes);

    // If check below fails, should manually dump the results with:
    //
    //   SETTINGS_MERGE_TEST_OUT=results.txt ./test_bitcoin --run_test=settings_tests/Merge
    //
    // And verify diff against previous results to make sure the changes are expected.
    //
    // Results file is formatted like:
    //
    //   <input> || GetSetting() | GetSettingsList() | OnlyHasDefaultSectionSetting()
    CHECK(out_sha_hex == "79db02d74e3e193196541b67c068b40ebd0c124a24b3ecbe9cbf7e85b1c4ba7a");
}

TEST_SUITE_END()
