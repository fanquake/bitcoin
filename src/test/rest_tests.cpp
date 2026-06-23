// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rest.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>

#include <test/util/framework.h>

#include <string>

TEST_SUITE_BEGIN(rest_tests)

FIXTURE_TEST_CASE(test_query_string, BasicTestingSetup)
{
    std::string param;
    RESTResponseFormat rf;
    // No query string
    rf = ParseDataFormat(param, "/rest/endpoint/someresource.json");
    CHECK(param == "/rest/endpoint/someresource");
    CHECK(rf == RESTResponseFormat::JSON);

    // Query string with single parameter
    rf = ParseDataFormat(param, "/rest/endpoint/someresource.bin?p1=v1");
    CHECK(param == "/rest/endpoint/someresource");
    CHECK(rf == RESTResponseFormat::BINARY);

    // Query string with multiple parameters
    rf = ParseDataFormat(param, "/rest/endpoint/someresource.hex?p1=v1&p2=v2");
    CHECK(param == "/rest/endpoint/someresource");
    CHECK(rf == RESTResponseFormat::HEX);

    // Incorrectly formed query string will not be handled
    rf = ParseDataFormat(param, "/rest/endpoint/someresource.json&p1=v1");
    CHECK(param == "/rest/endpoint/someresource.json&p1=v1");
    CHECK(rf == RESTResponseFormat::UNDEF);

    // Omitted data format with query string should return UNDEF and hide query string
    rf = ParseDataFormat(param, "/rest/endpoint/someresource?p1=v1");
    CHECK(param == "/rest/endpoint/someresource");
    CHECK(rf == RESTResponseFormat::UNDEF);

    // Data format specified after query string
    rf = ParseDataFormat(param, "/rest/endpoint/someresource?p1=v1.json");
    CHECK(param == "/rest/endpoint/someresource");
    CHECK(rf == RESTResponseFormat::UNDEF);
}
TEST_SUITE_END()
