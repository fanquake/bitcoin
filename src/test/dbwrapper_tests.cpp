// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <test/util/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/string.h>

#include <memory>
#include <ranges>

#include <test/util/framework.h>

using util::ToString;

TEST_SUITE_BEGIN(dbwrapper_tests)

FIXTURE_TEST_CASE(dbwrapper, BasicTestingSetup)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        constexpr size_t CACHE_SIZE{1_MiB};
        const fs::path path{m_args.GetDataDirBase() / "dbwrapper"};

        Obfuscation obfuscation;
        std::vector<std::pair<uint8_t, uint256>> key_values{};

        // Write values
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .wipe_data = true, .obfuscate = obfuscate}};
            CHECK(obfuscate == !dbw.IsEmpty());

            // Ensure that we're doing real obfuscation when obfuscate=true
            obfuscation = dbwrapper_private::GetObfuscation(dbw);
            CHECK(obfuscate == dbwrapper_private::GetObfuscation(dbw));

            for (uint8_t k{0}; k < 10; ++k) {
                uint8_t key{k};
                uint256 value{m_rng.rand256()};
                dbw.Write(key, value);
                key_values.emplace_back(key, value);
            }
        }

        // Verify that the obfuscation key is never obfuscated
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .obfuscate = false}};
            CHECK(obfuscation == dbwrapper_private::GetObfuscation(dbw));
        }

        // Read back the values
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .obfuscate = obfuscate}};

            // Ensure obfuscation is read back correctly
            CHECK(obfuscation == dbwrapper_private::GetObfuscation(dbw));
            CHECK(obfuscate == dbwrapper_private::GetObfuscation(dbw));

            // Verify all written values
            for (const auto& [key, expected_value] : key_values) {
                uint256 read_value{};
                CHECK(dbw.Read(key, read_value));
                CHECK(read_value == expected_value);
            }
        }
    }
}

FIXTURE_TEST_CASE(dbwrapper_basic_data, BasicTestingSetup)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_1_obfuscate_true" : "dbwrapper_1_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1_MiB, .memory_only = false, .wipe_data = true, .obfuscate = obfuscate});

        uint256 res;
        uint32_t res_uint_32;
        bool res_bool;

        // Ensure that we're doing real obfuscation when obfuscate=true
        CHECK(obfuscate == dbwrapper_private::GetObfuscation(dbw));

        //Simulate block raw data - "b + block hash"
        std::string key_block = "b" + m_rng.rand256().ToString();

        uint256 in_block = m_rng.rand256();
        dbw.Write(key_block, in_block);
        CHECK(dbw.Read(key_block, res));
        CHECK(res.ToString() == in_block.ToString());

        //Simulate file raw data - "f + file_number"
        std::string key_file = strprintf("f%04x", m_rng.rand32());

        uint256 in_file_info = m_rng.rand256();
        dbw.Write(key_file, in_file_info);
        CHECK(dbw.Read(key_file, res));
        CHECK(res.ToString() == in_file_info.ToString());

        //Simulate transaction raw data - "t + transaction hash"
        std::string key_transaction = "t" + m_rng.rand256().ToString();

        uint256 in_transaction = m_rng.rand256();
        dbw.Write(key_transaction, in_transaction);
        CHECK(dbw.Read(key_transaction, res));
        CHECK(res.ToString() == in_transaction.ToString());

        //Simulate UTXO raw data - "c + transaction hash"
        std::string key_utxo = "c" + m_rng.rand256().ToString();

        uint256 in_utxo = m_rng.rand256();
        dbw.Write(key_utxo, in_utxo);
        CHECK(dbw.Read(key_utxo, res));
        CHECK(res.ToString() == in_utxo.ToString());

        //Simulate last block file number - "l"
        uint8_t key_last_blockfile_number{'l'};
        uint32_t lastblockfilenumber = m_rng.rand32();
        dbw.Write(key_last_blockfile_number, lastblockfilenumber);
        CHECK(dbw.Read(key_last_blockfile_number, res_uint_32));
        CHECK(lastblockfilenumber == res_uint_32);

        //Simulate Is Reindexing - "R"
        uint8_t key_IsReindexing{'R'};
        bool isInReindexing = m_rng.randbool();
        dbw.Write(key_IsReindexing, isInReindexing);
        CHECK(dbw.Read(key_IsReindexing, res_bool));
        CHECK(isInReindexing == res_bool);

        //Simulate last block hash up to which UXTO covers - 'B'
        uint8_t key_lastblockhash_uxto{'B'};
        uint256 lastblock_hash = m_rng.rand256();
        dbw.Write(key_lastblockhash_uxto, lastblock_hash);
        CHECK(dbw.Read(key_lastblockhash_uxto, res));
        CHECK(lastblock_hash == res);

        //Simulate file raw data - "F + filename_number + filename"
        std::string file_option_tag = "F";
        uint8_t filename_length = m_rng.randbits(8);
        std::string filename = "randomfilename";
        std::string key_file_option = strprintf("%s%01x%s", file_option_tag, filename_length, filename);

        bool in_file_bool = m_rng.randbool();
        dbw.Write(key_file_option, in_file_bool);
        CHECK(dbw.Read(key_file_option, res_bool));
        CHECK(res_bool == in_file_bool);
    }
}

// Test batch operations
FIXTURE_TEST_CASE(dbwrapper_batch, BasicTestingSetup)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_batch_obfuscate_true" : "dbwrapper_batch_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1_MiB, .memory_only = true, .wipe_data = false, .obfuscate = obfuscate});

        uint8_t key{'i'};
        uint256 in = m_rng.rand256();
        uint8_t key2{'j'};
        uint256 in2 = m_rng.rand256();
        uint8_t key3{'k'};
        uint256 in3 = m_rng.rand256();

        uint256 res;
        CDBBatch batch(dbw);

        batch.Write(key, in);
        batch.Write(key2, in2);
        batch.Write(key3, in3);

        // Remove key3 before it's even been written
        batch.Erase(key3);

        dbw.WriteBatch(batch);

        CHECK(dbw.Read(key, res));
        CHECK(res.ToString() == in.ToString());
        CHECK(dbw.Read(key2, res));
        CHECK(res.ToString() == in2.ToString());

        // key3 should've never been written
        CHECK(dbw.Read(key3, res) == false);

        batch.Clear();
        batch.Write(key3, in3);
        dbw.WriteBatch(batch);

        CHECK(dbw.Read(key3, res));
        CHECK(res.ToString() == in3.ToString());
    }
}

FIXTURE_TEST_CASE(dbwrapper_iterator, BasicTestingSetup)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_iterator_obfuscate_true" : "dbwrapper_iterator_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1_MiB, .memory_only = true, .wipe_data = false, .obfuscate = obfuscate});

        // The two keys are intentionally chosen for ordering
        uint8_t key{'j'};
        uint256 in = m_rng.rand256();
        dbw.Write(key, in);
        uint8_t key2{'k'};
        uint256 in2 = m_rng.rand256();
        dbw.Write(key2, in2);

        std::unique_ptr<CDBIterator> it(dbw.NewIterator());

        // Be sure to seek past the obfuscation key (if it exists)
        it->Seek(key);

        // A failed key decode must not consume the current iterator entry.
        uint16_t key_too_large{0};
        CHECK(!it->GetKey(key_too_large));

        uint8_t key_res;

        REQUIRE(it->GetKey(key_res));
        CHECK(key_res == key);
        // A failed value decode must not leave the iterator's scratch stream dirty.
        std::pair<uint256, uint8_t> value_too_large;
        CHECK(!it->GetValue(value_too_large));

        uint256 val_res;
        REQUIRE(it->GetValue(val_res));
        CHECK(val_res.ToString() == in.ToString());

        it->Seek(key2);

        REQUIRE(it->GetKey(key_res));
        CHECK(key_res == key2);
        REQUIRE(it->GetValue(val_res));
        CHECK(val_res.ToString() == in2.ToString());

        it->Seek(key);

        REQUIRE(it->GetKey(key_res));
        CHECK(key_res == key);
        REQUIRE(it->GetValue(val_res));
        CHECK(val_res.ToString() == in.ToString());

        it->Next();

        REQUIRE(it->GetKey(key_res));
        CHECK(key_res == key2);
        REQUIRE(it->GetValue(val_res));
        CHECK(val_res.ToString() == in2.ToString());

        it->Next();
        CHECK(it->Valid() == false);
    }
}

// Test that we do not obfuscation if there is existing data.
FIXTURE_TEST_CASE(existing_data_no_obfuscate, BasicTestingSetup)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_no_obfuscate";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(DBParams{.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = false});
    uint8_t key{'k'};
    uint256 in = m_rng.rand256();
    uint256 res;

    dbw->Write(key, in);
    CHECK(dbw->Read(key, res));
    CHECK(res.ToString() == in.ToString());

    // Call the destructor to free leveldb LOCK
    dbw.reset();

    // Now, set up another wrapper that wants to obfuscate the same directory
    CDBWrapper odbw({.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = true});

    // Check that the key/val we wrote with unobfuscated wrapper exists and
    // is readable.
    uint256 res2;
    CHECK(odbw.Read(key, res2));
    CHECK(res2.ToString() == in.ToString());

    CHECK(!odbw.IsEmpty());
    CHECK(!dbwrapper_private::GetObfuscation(odbw)); // The key should be an empty string

    uint256 in2 = m_rng.rand256();
    uint256 res3;

    // Check that we can write successfully
    odbw.Write(key, in2);
    CHECK(odbw.Read(key, res3));
    CHECK(res3.ToString() == in2.ToString());
}

// Ensure that we start obfuscating during a reindex.
FIXTURE_TEST_CASE(existing_data_reindex, BasicTestingSetup)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_reindex";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(DBParams{.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = false});
    uint8_t key{'k'};
    uint256 in = m_rng.rand256();
    uint256 res;

    dbw->Write(key, in);
    CHECK(dbw->Read(key, res));
    CHECK(res.ToString() == in.ToString());

    // Call the destructor to free leveldb LOCK
    dbw.reset();

    // Simulate a -reindex by wiping the existing data store
    CDBWrapper odbw({.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = true, .obfuscate = true});

    // Check that the key/val we wrote with unobfuscated wrapper doesn't exist
    uint256 res2;
    CHECK(!odbw.Read(key, res2));
    CHECK(dbwrapper_private::GetObfuscation(odbw));

    uint256 in2 = m_rng.rand256();
    uint256 res3;

    // Check that we can write successfully
    odbw.Write(key, in2);
    CHECK(odbw.Read(key, res3));
    CHECK(res3.ToString() == in2.ToString());
}

FIXTURE_TEST_CASE(iterator_ordering, BasicTestingSetup)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_ordering";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1_MiB, .memory_only = true, .wipe_data = false, .obfuscate = false});
    for (int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (!(x & 1)) dbw.Write(key, value);
    }

    // Check that creating an iterator creates a snapshot
    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());

    for (unsigned int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (x & 1) dbw.Write(key, value);
    }

    for (const int seek_start : {0x00, 0x80}) {
        it->Seek((uint8_t)seek_start);
        for (unsigned int x=seek_start; x<255; ++x) {
            uint8_t key{};
            uint32_t value{};
            CHECK(it->Valid());
            if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                break;
            CHECK(it->GetKey(key));
            if (x & 1) {
                CHECK(static_cast<unsigned int>(key) == x + 1);
                continue;
            }
            CHECK(it->GetValue(value));
            CHECK(static_cast<unsigned int>(key) == x);
            CHECK(value == x*x);
            it->Next();
        }
        CHECK(!it->Valid());
    }
}

struct StringContentsSerializer {
    // Used to make two serialized objects the same while letting them have different lengths
    // This is a terrible idea
    std::string str;
    StringContentsSerializer() = default;
    explicit StringContentsSerializer(const std::string& inp) : str(inp) {}

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        for (size_t i = 0; i < str.size(); i++) {
            s << uint8_t(str[i]);
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        str.clear();
        uint8_t c{0};
        while (!s.empty()) {
            s >> c;
            str.push_back(c);
        }
    }
};

FIXTURE_TEST_CASE(iterator_string_ordering, BasicTestingSetup)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_string_ordering";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1_MiB, .memory_only = true, .wipe_data = false, .obfuscate = false});
    for (int x = 0; x < 10; ++x) {
        for (int y = 0; y < 10; ++y) {
            std::string key{ToString(x)};
            for (int z = 0; z < y; ++z)
                key += key;
            uint32_t value = x*x;
            dbw.Write(StringContentsSerializer{key}, value);
        }
    }

    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());
    for (const int seek_start : {0, 5}) {
        it->Seek(StringContentsSerializer{ToString(seek_start)});
        for (unsigned int x = seek_start; x < 10; ++x) {
            for (int y = 0; y < 10; ++y) {
                std::string exp_key{ToString(x)};
                for (int z = 0; z < y; ++z)
                    exp_key += exp_key;
                StringContentsSerializer key;
                uint32_t value{};
                CHECK(it->Valid());
                if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                    break;
                CHECK(it->GetKey(key));
                CHECK(it->GetValue(value));
                CHECK(key.str == exp_key);
                CHECK(value == x*x);
                it->Next();
            }
        }
        CHECK(!it->Valid());
    }
}

FIXTURE_TEST_CASE(unicodepath, BasicTestingSetup)
{
    // Attempt to create a database with a UTF8 character in the path.
    // On Windows this test will fail if the directory is created using
    // the ANSI CreateDirectoryA call and the code page isn't UTF8.
    // It will succeed if created with CreateDirectoryW.
    fs::path ph = m_args.GetDataDirBase() / "test_runner_₿_🏃_20191128_104644";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1_MiB});

    fs::path lockPath = ph / "LOCK";
    CHECK(fs::exists(lockPath));
}


TEST_SUITE_END()
