// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compressor.h>
#include <script/script.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <cstdint>

#include <test/util/framework.h>

// amounts 0.00000001 .. 0.00100000
#define NUM_MULTIPLES_UNIT 100000

// amounts 0.01 .. 100.00
#define NUM_MULTIPLES_CENT 10000

// amounts 1 .. 10000
#define NUM_MULTIPLES_1BTC 10000

// amounts 50 .. 21000000
#define NUM_MULTIPLES_50BTC 420000

TEST_SUITE_BEGIN(compress_tests)

bool static TestEncode(uint64_t in) {
    return in == DecompressAmount(CompressAmount(in));
}

bool static TestDecode(uint64_t in) {
    return in == CompressAmount(DecompressAmount(in));
}

bool static TestPair(uint64_t dec, uint64_t enc) {
    return CompressAmount(dec) == enc &&
           DecompressAmount(enc) == dec;
}

FIXTURE_TEST_CASE(compress_amounts, BasicTestingSetup)
{
    CHECK(TestPair(            0,       0x0));
    CHECK(TestPair(            1,       0x1));
    CHECK(TestPair(         CENT,       0x7));
    CHECK(TestPair(         COIN,       0x9));
    CHECK(TestPair(      50*COIN,      0x32));
    CHECK(TestPair(21000000*COIN, 0x1406f40));

    for (uint64_t i = 1; i <= NUM_MULTIPLES_UNIT; i++)
        CHECK(TestEncode(i));

    for (uint64_t i = 1; i <= NUM_MULTIPLES_CENT; i++)
        CHECK(TestEncode(i * CENT));

    for (uint64_t i = 1; i <= NUM_MULTIPLES_1BTC; i++)
        CHECK(TestEncode(i * COIN));

    for (uint64_t i = 1; i <= NUM_MULTIPLES_50BTC; i++)
        CHECK(TestEncode(i * 50 * COIN));

    for (uint64_t i = 0; i < 100000; i++)
        CHECK(TestDecode(i));
}

FIXTURE_TEST_CASE(compress_script_to_ckey_id, BasicTestingSetup)
{
    // case CKeyID
    CKey key = GenerateRandomKey();
    CPubKey pubkey = key.GetPubKey();

    CScript script = CScript() << OP_DUP << OP_HASH160 << ToByteVector(pubkey.GetID()) << OP_EQUALVERIFY << OP_CHECKSIG;
    CHECK(script.size() == 25U);

    CompressedScript out;
    bool done = CompressScript(script, out);
    CHECK(done == true);

    // Check compressed script
    CHECK(out.size() == 21U);
    CHECK(out[0] == 0x00);
    CHECK(memcmp(out.data() + 1, script.data() + 3, 20) == 0); // compare the 20 relevant chars of the CKeyId in the script
}

FIXTURE_TEST_CASE(compress_script_to_cscript_id, BasicTestingSetup)
{
    // case CScriptID
    CScript script, redeemScript;
    script << OP_HASH160 << ToByteVector(CScriptID(redeemScript)) << OP_EQUAL;
    CHECK(script.size() == 23U);

    CompressedScript out;
    bool done = CompressScript(script, out);
    CHECK(done == true);

    // Check compressed script
    CHECK(out.size() == 21U);
    CHECK(out[0] == 0x01);
    CHECK(memcmp(out.data() + 1, script.data() + 2, 20) == 0); // compare the 20 relevant chars of the CScriptId in the script
}

FIXTURE_TEST_CASE(compress_script_to_compressed_pubkey_id, BasicTestingSetup)
{
    CKey key = GenerateRandomKey(); // case compressed PubKeyID

    CScript script = CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG; // COMPRESSED_PUBLIC_KEY_SIZE (33)
    CHECK(script.size() == 35U);

    CompressedScript out;
    bool done = CompressScript(script, out);
    CHECK(done == true);

    // Check compressed script
    CHECK(out.size() == 33U);
    CHECK(memcmp(out.data(), script.data() + 1, 1) == 0);
    CHECK(memcmp(out.data() + 1, script.data() + 2, 32) == 0); // compare the 32 chars of the compressed CPubKey
}

FIXTURE_TEST_CASE(compress_script_to_uncompressed_pubkey_id, BasicTestingSetup)
{
    CKey key = GenerateRandomKey(/*compressed=*/false); // case uncompressed PubKeyID
    CScript script =  CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG; // PUBLIC_KEY_SIZE (65)
    CHECK(script.size() == 67U);                   // 1 char code + 65 char pubkey + OP_CHECKSIG

    CompressedScript out;
    bool done = CompressScript(script, out);
    CHECK(done == true);

    // Check compressed script
    CHECK(out.size() == 33U);
    CHECK(memcmp(out.data() + 1, script.data() + 2, 32) == 0); // first 32 chars of CPubKey are copied into out[1:]
    CHECK(out[0] == (0x04 | (script[65] & 0x01))); // least significant bit (lsb) of last char of pubkey is mapped into out[0]
}

FIXTURE_TEST_CASE(compress_p2pk_scripts_not_on_curve, BasicTestingSetup)
{
    XOnlyPubKey x_not_on_curve;
    do {
        x_not_on_curve = XOnlyPubKey(m_rng.randbytes(32));
    } while (x_not_on_curve.IsFullyValid());

    // Check that P2PK script with uncompressed pubkey [=> OP_PUSH65 <0x04 .....> OP_CHECKSIG]
    // which is not fully valid (i.e. point is not on curve) can't be compressed
    std::vector<unsigned char> pubkey_raw(65, 0);
    pubkey_raw[0] = 4;
    std::copy(x_not_on_curve.begin(), x_not_on_curve.end(), &pubkey_raw[1]);
    CPubKey pubkey_not_on_curve(pubkey_raw);
    assert(pubkey_not_on_curve.IsValid());
    assert(!pubkey_not_on_curve.IsFullyValid());
    CScript script = CScript() << ToByteVector(pubkey_not_on_curve) << OP_CHECKSIG;
    CHECK(script.size() == 67U);

    CompressedScript out;
    bool done = CompressScript(script, out);
    CHECK(done == false);

    // Check that compressed P2PK script with uncompressed pubkey that is not fully
    // valid (i.e. x coordinate of the pubkey is not on curve) can't be decompressed
    CompressedScript compressed_script(x_not_on_curve.begin(), x_not_on_curve.end());
    for (unsigned int compression_id : {4, 5}) {
        CScript uncompressed_script;
        bool success = DecompressScript(uncompressed_script, compression_id, compressed_script);
        CHECK(success == false);
    }
}

TEST_SUITE_END()
