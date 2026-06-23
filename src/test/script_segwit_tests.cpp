// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/script.h>
#include <test/util/setup_common.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(script_segwit_tests)

FIXTURE_TEST_CASE(IsPayToWitnessScriptHash_Valid, BasicTestingSetup)
{
    uint256 dummy;
    CScript p2wsh;
    p2wsh << OP_0 << ToByteVector(dummy);
    CHECK(p2wsh.IsPayToWitnessScriptHash());

    std::vector<unsigned char> bytes = {OP_0, 32};
    bytes.insert(bytes.end(), 32, 0);
    CHECK(CScript(bytes.begin(), bytes.end()).IsPayToWitnessScriptHash());
}

FIXTURE_TEST_CASE(IsPayToWitnessScriptHash_Invalid_NotOp0, BasicTestingSetup)
{
    uint256 dummy;
    CScript notp2wsh;
    notp2wsh << OP_1 << ToByteVector(dummy);
    CHECK(!notp2wsh.IsPayToWitnessScriptHash());
}

FIXTURE_TEST_CASE(IsPayToWitnessScriptHash_Invalid_Size, BasicTestingSetup)
{
    uint160 dummy;
    CScript notp2wsh;
    notp2wsh << OP_0 << ToByteVector(dummy);
    CHECK(!notp2wsh.IsPayToWitnessScriptHash());
}

FIXTURE_TEST_CASE(IsPayToWitnessScriptHash_Invalid_Nop, BasicTestingSetup)
{
    uint256 dummy;
    CScript notp2wsh;
    notp2wsh << OP_0 << OP_NOP << ToByteVector(dummy);
    CHECK(!notp2wsh.IsPayToWitnessScriptHash());
}

FIXTURE_TEST_CASE(IsPayToWitnessScriptHash_Invalid_EmptyScript, BasicTestingSetup)
{
    CScript notp2wsh;
    CHECK(!notp2wsh.IsPayToWitnessScriptHash());
}

FIXTURE_TEST_CASE(IsPayToWitnessScriptHash_Invalid_Pushdata, BasicTestingSetup)
{
    // A script is not P2WSH if OP_PUSHDATA is used to push the hash.
    std::vector<unsigned char> bytes = {OP_0, OP_PUSHDATA1, 32};
    bytes.insert(bytes.end(), 32, 0);
    CHECK(!CScript(bytes.begin(), bytes.end()).IsPayToWitnessScriptHash());

    bytes = {OP_0, OP_PUSHDATA2, 32, 0};
    bytes.insert(bytes.end(), 32, 0);
    CHECK(!CScript(bytes.begin(), bytes.end()).IsPayToWitnessScriptHash());

    bytes = {OP_0, OP_PUSHDATA4, 32, 0, 0, 0};
    bytes.insert(bytes.end(), 32, 0);
    CHECK(!CScript(bytes.begin(), bytes.end()).IsPayToWitnessScriptHash());
}

namespace {

bool IsExpectedWitnessProgram(const CScript& script, const int expectedVersion, const std::vector<unsigned char>& expectedProgram)
{
    int actualVersion;
    std::vector<unsigned char> actualProgram;
    if (!script.IsWitnessProgram(actualVersion, actualProgram)) {
        return false;
    }
    CHECK(actualVersion == expectedVersion);
    CHECK(actualProgram == expectedProgram);
    return true;
}

bool IsNoWitnessProgram(const CScript& script)
{
    int dummyVersion;
    std::vector<unsigned char> dummyProgram;
    return !script.IsWitnessProgram(dummyVersion, dummyProgram);
}

} // anonymous namespace

FIXTURE_TEST_CASE(IsWitnessProgram_Valid, BasicTestingSetup)
{
    // Witness programs have a minimum data push of 2 bytes.
    std::vector<unsigned char> program = {42, 18};
    CScript wit;
    wit << OP_0 << program;
    CHECK(IsExpectedWitnessProgram(wit, 0, program));

    wit.clear();
    // Witness programs have a maximum data push of 40 bytes.
    program.resize(40);
    wit << OP_16 << program;
    CHECK(IsExpectedWitnessProgram(wit, 16, program));

    program.resize(32);
    std::vector<unsigned char> bytes = {OP_5, static_cast<unsigned char>(program.size())};
    bytes.insert(bytes.end(), program.begin(), program.end());
    CHECK(IsExpectedWitnessProgram(CScript(bytes.begin(), bytes.end()), 5, program));
}

FIXTURE_TEST_CASE(IsWitnessProgram_Invalid_Version, BasicTestingSetup)
{
    std::vector<unsigned char> program(10);
    CScript nowit;
    nowit << OP_1NEGATE << program;
    CHECK(IsNoWitnessProgram(nowit));
}

FIXTURE_TEST_CASE(IsWitnessProgram_Invalid_Size, BasicTestingSetup)
{
    std::vector<unsigned char> program(1);
    CScript nowit;
    nowit << OP_0 << program;
    CHECK(IsNoWitnessProgram(nowit));

    nowit.clear();
    program.resize(41);
    nowit << OP_0 << program;
    CHECK(IsNoWitnessProgram(nowit));
}

FIXTURE_TEST_CASE(IsWitnessProgram_Invalid_Nop, BasicTestingSetup)
{
    std::vector<unsigned char> program(10);
    CScript nowit;
    nowit << OP_0 << OP_NOP << program;
    CHECK(IsNoWitnessProgram(nowit));
}

FIXTURE_TEST_CASE(IsWitnessProgram_Invalid_EmptyScript, BasicTestingSetup)
{
    CScript nowit;
    CHECK(IsNoWitnessProgram(nowit));
}

FIXTURE_TEST_CASE(IsWitnessProgram_Invalid_Pushdata, BasicTestingSetup)
{
    // A script is no witness program if OP_PUSHDATA is used to push the hash.
    std::vector<unsigned char> bytes = {OP_0, OP_PUSHDATA1, 32};
    bytes.insert(bytes.end(), 32, 0);
    CHECK(IsNoWitnessProgram(CScript(bytes.begin(), bytes.end())));

    bytes = {OP_0, OP_PUSHDATA2, 32, 0};
    bytes.insert(bytes.end(), 32, 0);
    CHECK(IsNoWitnessProgram(CScript(bytes.begin(), bytes.end())));

    bytes = {OP_0, OP_PUSHDATA4, 32, 0, 0, 0};
    bytes.insert(bytes.end(), 32, 0);
    CHECK(IsNoWitnessProgram(CScript(bytes.begin(), bytes.end())));
}

TEST_SUITE_END()
