// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <tinyformat.h>
#include <util/translation.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(translation_tests)

static TranslateFn translate{[](const char * str) {  return strprintf("t(%s)", str);  }};

// Custom translation function _t(), similar to _() but internal to this test.
consteval auto _t(util::TranslatedLiteral str)
{
    str.translate_fn = &translate;
    return str;
}

TEST_CASE(translation_namedparams)
{
    bilingual_str arg{"original", "translated"};
    bilingual_str result{strprintf(_t("original [%s]"), arg)};
    CHECK(result.original == "original [original]");
    CHECK(result.translated == "t(original [translated])");

    util::TranslatedLiteral arg2{"original", &translate};
    bilingual_str result2{strprintf(_t("original [%s]"), arg2)};
    CHECK(result2.original == "original [original]");
    CHECK(result2.translated == "t(original [t(original)])");
}

TEST_SUITE_END()
