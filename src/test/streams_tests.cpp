// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flatfile.h>
#include <node/blockstorage.h>
#include <streams.h>
#include <test/util/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/obfuscation.h>
#include <util/strencodings.h>

#include <test/util/framework.h>

using namespace std::string_literals;
using namespace util::hex_literals;

TEST_SUITE_BEGIN(streams_tests)

// Check optimized obfuscation with random offsets and sizes to ensure proper
// handling of key wrapping. Also verify it roundtrips.
FIXTURE_TEST_CASE(xor_random_chunks, BasicTestingSetup)
{
    auto apply_random_xor_chunks{[&](std::span<std::byte> target, const Obfuscation& obfuscation) {
        for (size_t offset{0}; offset < target.size();) {
            const size_t chunk_size{1 + m_rng.randrange(target.size() - offset)};
            obfuscation(target.subspan(offset, chunk_size), offset);
            offset += chunk_size;
        }
    }};

    for (size_t test{0}; test < 100; ++test) {
        const size_t write_size{1 + m_rng.randrange(100U)};
        const std::vector original{m_rng.randbytes<std::byte>(write_size)};
        std::vector roundtrip{original};

        const auto key_bytes{m_rng.randbool() ? m_rng.randbytes<Obfuscation::KEY_SIZE>() : std::array<std::byte, Obfuscation::KEY_SIZE>{}};
        const Obfuscation obfuscation{key_bytes};
        apply_random_xor_chunks(roundtrip, obfuscation);
        CHECK(roundtrip.size() == original.size());
        for (size_t i{0}; i < original.size(); ++i) {
            CHECK(roundtrip[i] == (original[i] ^ key_bytes[i % Obfuscation::KEY_SIZE]));
        }

        apply_random_xor_chunks(roundtrip, obfuscation);
        CHECK_EQUAL_RANGES(roundtrip, original);
  }
}

FIXTURE_TEST_CASE(obfuscation_hexkey, BasicTestingSetup)
{
    const auto key_bytes{m_rng.randbytes<Obfuscation::KEY_SIZE>()};

    const Obfuscation obfuscation{key_bytes};
    CHECK(obfuscation.HexKey() == HexStr(key_bytes));
}

FIXTURE_TEST_CASE(obfuscation_serialize, BasicTestingSetup)
{
    Obfuscation obfuscation{};
    CHECK(!obfuscation);

    // Test loading a key.
    std::vector key_in{m_rng.randbytes<std::byte>(Obfuscation::KEY_SIZE)};
    DataStream ds_in;
    ds_in << key_in;
    CHECK(ds_in.size() == 1 + Obfuscation::KEY_SIZE); // serialized as a vector
    ds_in >> obfuscation;

    // Test saving the key.
    std::vector<std::byte> key_out;
    DataStream ds_out;
    ds_out << obfuscation;
    ds_out >> key_out;

    // Make sure saved key is the same.
    CHECK_EQUAL_RANGES(key_in, key_out);
}

FIXTURE_TEST_CASE(obfuscation_empty, BasicTestingSetup)
{
    const Obfuscation null_obf{};
    CHECK(!null_obf);

    const Obfuscation non_null_obf{"ff00ff00ff00ff00"_hex};
    CHECK(non_null_obf);
}

FIXTURE_TEST_CASE(streams_scoped_data_stream_usage, BasicTestingSetup)
{
    DataStream stream{};
    {
        ScopedDataStreamUsage usage{stream};
        stream << uint8_t{42};
        CHECK(stream.size() > 0U);
    }
    CHECK(stream.empty());

    {
        ScopedDataStreamUsage usage{stream};
        stream << uint16_t{42};
        CHECK(stream.size() > 0U);
    }
    CHECK(stream.empty());
}

FIXTURE_TEST_CASE(xor_file, BasicTestingSetup)
{
    fs::path xor_path{m_args.GetDataDirBase() / "test_xor.bin"};
    auto raw_file{[&](const auto& mode) { return fsbridge::fopen(xor_path, mode); }};
    const std::vector<uint8_t> test1{1, 2, 3};
    const std::vector<uint8_t> test2{4, 5};
    const Obfuscation obfuscation{"ff00ff00ff00ff00"_hex};

    {
        // Check errors for missing file
        AutoFile xor_file{raw_file("rb"), obfuscation};
        CHECK_EXCEPTION(xor_file << std::byte{}, std::ios_base::failure, HasReason{"AutoFile::write: file handle is nullptr"});
        CHECK_EXCEPTION(xor_file >> std::byte{}, std::ios_base::failure, HasReason{"AutoFile::read: file handle is nullptr"});
        CHECK_EXCEPTION(xor_file.ignore(1), std::ios_base::failure, HasReason{"AutoFile::ignore: file handle is nullptr"});
        CHECK_EXCEPTION(xor_file.size(), std::ios_base::failure, HasReason{"AutoFile::size: file handle is nullptr"});
    }
    {
#ifdef __MINGW64__
        // Temporary workaround for https://github.com/bitcoin/bitcoin/issues/30210
        const char* mode = "wb";
#else
        const char* mode = "wbx";
#endif
        AutoFile xor_file{raw_file(mode), obfuscation};
        xor_file << test1 << test2;
        CHECK(xor_file.size() == 7);
        REQUIRE(xor_file.fclose() == 0);
    }
    {
        // Read raw from disk
        AutoFile non_xor_file{raw_file("rb")};
        std::vector<std::byte> raw(7);
        non_xor_file >> std::span{raw};
        CHECK(HexStr(raw) == "fc01fd03fd04fa");
        // Check that no padding exists
        CHECK_EXCEPTION(non_xor_file.ignore(1), std::ios_base::failure, HasReason{"AutoFile::ignore: end of file"});
        CHECK(non_xor_file.size() == 7);
    }
    {
        AutoFile xor_file{raw_file("rb"), obfuscation};
        std::vector<std::byte> read1, read2;
        xor_file >> read1 >> read2;
        CHECK(HexStr(read1) == HexStr(test1));
        CHECK(HexStr(read2) == HexStr(test2));
        // Check that eof was reached
        CHECK_EXCEPTION(xor_file >> std::byte{}, std::ios_base::failure, HasReason{"AutoFile::read: end of file"});
        CHECK(xor_file.size() == 7);
    }
    {
        AutoFile xor_file{raw_file("rb"), obfuscation};
        std::vector<std::byte> read2;
        // Check that ignore works
        xor_file.ignore(4);
        xor_file >> read2;
        CHECK(HexStr(read2) == HexStr(test2));
        // Check that ignore and read fail now
        CHECK_EXCEPTION(xor_file.ignore(1), std::ios_base::failure, HasReason{"AutoFile::ignore: end of file"});
        CHECK_EXCEPTION(xor_file >> std::byte{}, std::ios_base::failure, HasReason{"AutoFile::read: end of file"});
        CHECK(xor_file.size() == 7);
    }
}

FIXTURE_TEST_CASE(streams_vector_writer, BasicTestingSetup)
{
    unsigned char a(1);
    unsigned char b(2);
    unsigned char bytes[] = {3, 4, 5, 6};
    std::vector<unsigned char> vch;

    // Each test runs twice. Serializing a second time at the same starting
    // point should yield the same results, even if the first test grew the
    // vector.

    VectorWriter{vch, 0, a, b};
    CHECK((vch == std::vector<unsigned char>{{1, 2}}));
    VectorWriter{vch, 0, a, b};
    CHECK((vch == std::vector<unsigned char>{{1, 2}}));
    vch.clear();

    VectorWriter{vch, 2, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 1, 2}}));
    VectorWriter{vch, 2, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 1, 2}}));
    vch.clear();

    vch.resize(5, 0);
    VectorWriter{vch, 2, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 1, 2, 0}}));
    VectorWriter{vch, 2, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 1, 2, 0}}));
    vch.clear();

    vch.resize(4, 0);
    VectorWriter{vch, 3, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 0, 1, 2}}));
    VectorWriter{vch, 3, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 0, 1, 2}}));
    vch.clear();

    vch.resize(4, 0);
    VectorWriter{vch, 4, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 0, 0, 1, 2}}));
    VectorWriter{vch, 4, a, b};
    CHECK((vch == std::vector<unsigned char>{{0, 0, 0, 0, 1, 2}}));
    vch.clear();

    VectorWriter{vch, 0, bytes};
    CHECK((vch == std::vector<unsigned char>{{3, 4, 5, 6}}));
    VectorWriter{vch, 0, bytes};
    CHECK((vch == std::vector<unsigned char>{{3, 4, 5, 6}}));
    vch.clear();

    vch.resize(4, 8);
    VectorWriter{vch, 2, a, bytes, b};
    CHECK((vch == std::vector<unsigned char>{{8, 8, 1, 3, 4, 5, 6, 2}}));
    VectorWriter{vch, 2, a, bytes, b};
    CHECK((vch == std::vector<unsigned char>{{8, 8, 1, 3, 4, 5, 6, 2}}));
    vch.clear();
}

FIXTURE_TEST_CASE(streams_span_writer, BasicTestingSetup)
{
    unsigned char a(1);
    unsigned char b(2);
    unsigned char bytes[] = {3, 4, 5, 6};
    std::array<std::byte, 8> arr{};

    // Test operator<<
    SpanWriter writer{arr};
    writer << a << b;
    CHECK(HexStr(arr) == "0102000000000000");

    // Use variadic constructor and write to subspan.
    SpanWriter{std::span{arr}.subspan(2), a, bytes, b};
    CHECK(HexStr(arr) == "0102010304050602");

    // Writing past the end throws
    std::array<std::byte, 1> small{};
    CHECK_THROWS_AS(SpanWriter(std::span{small}, a, b), std::ios_base::failure);
    CHECK_THROWS_AS(SpanWriter(std::span{small}) << a << b, std::ios_base::failure);
}

FIXTURE_TEST_CASE(streams_vector_reader, BasicTestingSetup)
{
    std::vector<unsigned char> vch = {1, 255, 3, 4, 5, 6};

    SpanReader reader{vch};
    CHECK(reader.size() == 6U);
    CHECK(!reader.empty());

    // Read a single byte as an unsigned char.
    unsigned char a;
    reader >> a;
    CHECK(a == 1);
    CHECK(reader.size() == 5U);
    CHECK(!reader.empty());

    // Read a single byte as a int8_t.
    int8_t b;
    reader >> b;
    CHECK(b == -1);
    CHECK(reader.size() == 4U);
    CHECK(!reader.empty());

    // Read a 4 bytes as an unsigned int.
    unsigned int c;
    reader >> c;
    CHECK(c == 100992003U); // 3,4,5,6 in little-endian base-256
    CHECK(reader.size() == 0U);
    CHECK(reader.empty());

    // Reading after end of byte vector throws an error.
    signed int d;
    CHECK_THROWS_AS(reader >> d, std::ios_base::failure);

    // Read a 4 bytes as a signed int from the beginning of the buffer.
    SpanReader new_reader{vch};
    new_reader >> d;
    CHECK(d == 67370753); // 1,255,3,4 in little-endian base-256
    CHECK(new_reader.size() == 2U);
    CHECK(!new_reader.empty());

    // Reading after end of byte vector throws an error even if the reader is
    // not totally empty.
    CHECK_THROWS_AS(new_reader >> d, std::ios_base::failure);
}

FIXTURE_TEST_CASE(streams_vector_reader_rvalue, BasicTestingSetup)
{
    std::vector<uint8_t> data{0x82, 0xa7, 0x31};
    SpanReader reader{data};
    uint32_t varint = 0;
    // Deserialize into r-value
    reader >> VARINT(varint);
    CHECK(varint == 54321U);
    CHECK(reader.empty());
}

FIXTURE_TEST_CASE(bitstream_reader_writer, BasicTestingSetup)
{
    DataStream data{};

    BitStreamWriter bit_writer{data};
    bit_writer.Write(0, 1);
    bit_writer.Write(2, 2);
    bit_writer.Write(6, 3);
    bit_writer.Write(11, 4);
    bit_writer.Write(1, 5);
    bit_writer.Write(32, 6);
    bit_writer.Write(7, 7);
    bit_writer.Write(30497, 16);
    bit_writer.Flush();

    DataStream data_copy{data};
    uint32_t serialized_int1;
    data >> serialized_int1;
    CHECK(serialized_int1 == uint32_t{0x7700C35A}); // NOTE: Serialized as LE
    uint16_t serialized_int2;
    data >> serialized_int2;
    CHECK(serialized_int2 == uint16_t{0x1072}); // NOTE: Serialized as LE

    BitStreamReader bit_reader{data_copy};
    CHECK(bit_reader.Read(1) == 0U);
    CHECK(bit_reader.Read(2) == 2U);
    CHECK(bit_reader.Read(3) == 6U);
    CHECK(bit_reader.Read(4) == 11U);
    CHECK(bit_reader.Read(5) == 1U);
    CHECK(bit_reader.Read(6) == 32U);
    CHECK(bit_reader.Read(7) == 7U);
    CHECK(bit_reader.Read(16) == 30497U);
    CHECK_THROWS_AS(bit_reader.Read(8), std::ios_base::failure);
}

FIXTURE_TEST_CASE(streams_serializedata_xor, BasicTestingSetup)
{
    // Degenerate case
    {
        DataStream ds{};
        Obfuscation{}(ds);
        CHECK(""s == ds.str());
    }

    {
        const Obfuscation obfuscation{"ffffffffffffffff"_hex};

        DataStream ds{"0ff0"_hex};
        obfuscation(ds);
        CHECK("\xf0\x0f"s == ds.str());
    }

    {
        const Obfuscation obfuscation{"ff0fff0fff0fff0f"_hex};

        DataStream ds{"f00f"_hex};
        obfuscation(ds);
        CHECK("\x0f\x00"s == ds.str());
    }
}

FIXTURE_TEST_CASE(streams_buffered_file, BasicTestingSetup)
{
    fs::path streams_test_filename = m_args.GetDataDirBase() / "streams_test_tmp";
    AutoFile file{fsbridge::fopen(streams_test_filename, "w+b")};

    // The value at each offset is the offset.
    for (uint8_t j = 0; j < 40; ++j) {
        file << j;
    }
    file.seek(0, SEEK_SET);

    // The buffer size (second arg) must be greater than the rewind
    // amount (third arg).
    try {
        BufferedFile bfbad{file, 25, 25};
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(strstr(e.what(),
                        "Rewind limit must be less than buffer size") != nullptr);
    }

    // The buffer is 25 bytes, allow rewinding 10 bytes.
    BufferedFile bf{file, 25, 10};
    CHECK(!bf.eof());

    uint8_t i;
    bf >> i;
    CHECK(i == 0);
    bf >> i;
    CHECK(i == 1);

    // After reading bytes 0 and 1, we're positioned at 2.
    CHECK(bf.GetPos() == 2U);

    // Rewind to offset 0, ok (within the 10 byte window).
    CHECK(bf.SetPos(0));
    bf >> i;
    CHECK(i == 0);

    // We can go forward to where we've been, but beyond may fail.
    CHECK(bf.SetPos(2));
    bf >> i;
    CHECK(i == 2);

    // If you know the maximum number of bytes that should be
    // read to deserialize the variable, you can limit the read
    // extent. The current file offset is 3, so the following
    // SetLimit() allows zero bytes to be read.
    CHECK(bf.SetLimit(3));
    try {
        bf >> i;
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(strstr(e.what(),
                           "Attempt to position past buffer limit") != nullptr);
    }
    // The default argument removes the limit completely.
    CHECK(bf.SetLimit());
    // The read position should still be at 3 (no change).
    CHECK(bf.GetPos() == 3U);

    // Read from current offset, 3, forward until position 10.
    for (uint8_t j = 3; j < 10; ++j) {
        bf >> i;
        CHECK(i == j);
    }
    CHECK(bf.GetPos() == 10U);

    // We're guaranteed (just barely) to be able to rewind to zero.
    CHECK(bf.SetPos(0));
    CHECK(bf.GetPos() == 0U);
    bf >> i;
    CHECK(i == 0);

    // We can set the position forward again up to the farthest
    // into the stream we've been, but no farther. (Attempting
    // to go farther may succeed, but it's not guaranteed.)
    CHECK(bf.SetPos(10));
    bf >> i;
    CHECK(i == 10);
    CHECK(bf.GetPos() == 11U);

    // Now it's only guaranteed that we can rewind to offset 1
    // (current read position, 11, minus rewind amount, 10).
    CHECK(bf.SetPos(1));
    CHECK(bf.GetPos() == 1U);
    bf >> i;
    CHECK(i == 1);

    // We can stream into large variables, even larger than
    // the buffer size.
    CHECK(bf.SetPos(11));
    {
        uint8_t a[40 - 11];
        bf >> a;
        for (uint8_t j = 0; j < sizeof(a); ++j) {
            CHECK(a[j] == 11 + j);
        }
    }
    CHECK(bf.GetPos() == 40U);

    // We've read the entire file, the next read should throw.
    try {
        bf >> i;
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(strstr(e.what(),
                        "BufferedFile::Fill: end of file") != nullptr);
    }
    // Attempting to read beyond the end sets the EOF indicator.
    CHECK(bf.eof());

    // Still at offset 40, we can go back 10, to 30.
    CHECK(bf.GetPos() == 40U);
    CHECK(bf.SetPos(30));
    bf >> i;
    CHECK(i == 30);
    CHECK(bf.GetPos() == 31U);

    // We're too far to rewind to position zero.
    CHECK(!bf.SetPos(0));
    // But we should now be positioned at least as far back as allowed
    // by the rewind window (relative to our farthest read position, 40).
    CHECK(bf.GetPos() <= 30U);

    REQUIRE(file.fclose() == 0);

    fs::remove(streams_test_filename);
}

FIXTURE_TEST_CASE(streams_buffered_file_skip, BasicTestingSetup)
{
    fs::path streams_test_filename = m_args.GetDataDirBase() / "streams_test_tmp";
    AutoFile file{fsbridge::fopen(streams_test_filename, "w+b")};
    // The value at each offset is the byte offset (e.g. byte 1 in the file has the value 0x01).
    for (uint8_t j = 0; j < 40; ++j) {
        file << j;
    }
    file.seek(0, SEEK_SET);

    // The buffer is 25 bytes, allow rewinding 10 bytes.
    BufferedFile bf{file, 25, 10};

    uint8_t i;
    // This is like bf >> (7-byte-variable), in that it will cause data
    // to be read from the file into memory, but it's not copied to us.
    bf.SkipTo(7);
    CHECK(bf.GetPos() == 7U);
    bf >> i;
    CHECK(i == 7);

    // The bytes in the buffer up to offset 7 are valid and can be read.
    CHECK(bf.SetPos(0));
    bf >> i;
    CHECK(i == 0);
    bf >> i;
    CHECK(i == 1);

    bf.SkipTo(11);
    bf >> i;
    CHECK(i == 11);

    // SkipTo() honors the transfer limit; we can't position beyond the limit.
    bf.SetLimit(13);
    try {
        bf.SkipTo(14);
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(strstr(e.what(), "Attempt to position past buffer limit") != nullptr);
    }

    // We can position exactly to the transfer limit.
    bf.SkipTo(13);
    CHECK(bf.GetPos() == 13U);

    REQUIRE(file.fclose() == 0);
    fs::remove(streams_test_filename);
}

FIXTURE_TEST_CASE(streams_buffered_file_rand, BasicTestingSetup)
{
    // Make this test deterministic.
    SeedRandomForTest(SeedRand::ZEROS);

    fs::path streams_test_filename = m_args.GetDataDirBase() / "streams_test_tmp";
    for (int rep = 0; rep < 50; ++rep) {
        AutoFile file{fsbridge::fopen(streams_test_filename, "w+b")};
        size_t fileSize = m_rng.randrange(256);
        for (uint8_t i = 0; i < fileSize; ++i) {
            file << i;
        }
        file.seek(0, SEEK_SET);

        size_t bufSize = m_rng.randrange(300) + 1;
        size_t rewindSize = m_rng.randrange(bufSize);
        BufferedFile bf{file, bufSize, rewindSize};
        size_t currentPos = 0;
        size_t maxPos = 0;
        for (int step = 0; step < 100; ++step) {
            if (currentPos >= fileSize)
                break;

            // We haven't read to the end of the file yet.
            CHECK(!bf.eof());
            CHECK(bf.GetPos() == currentPos);

            // Pretend the file consists of a series of objects of varying
            // sizes; the boundaries of the objects can interact arbitrarily
            // with the CBufferFile's internal buffer. These first three
            // cases simulate objects of various sizes (1, 2, 5 bytes).
            switch (m_rng.randrange(6)) {
            case 0: {
                uint8_t a[1];
                if (currentPos + 1 > fileSize)
                    continue;
                bf.SetLimit(currentPos + 1);
                bf >> a;
                for (uint8_t i = 0; i < 1; ++i) {
                    CHECK(static_cast<size_t>(a[i]) == currentPos);
                    currentPos++;
                }
                break;
            }
            case 1: {
                uint8_t a[2];
                if (currentPos + 2 > fileSize)
                    continue;
                bf.SetLimit(currentPos + 2);
                bf >> a;
                for (uint8_t i = 0; i < 2; ++i) {
                    CHECK(static_cast<size_t>(a[i]) == currentPos);
                    currentPos++;
                }
                break;
            }
            case 2: {
                uint8_t a[5];
                if (currentPos + 5 > fileSize)
                    continue;
                bf.SetLimit(currentPos + 5);
                bf >> a;
                for (uint8_t i = 0; i < 5; ++i) {
                    CHECK(static_cast<size_t>(a[i]) == currentPos);
                    currentPos++;
                }
                break;
            }
            case 3: {
                // SkipTo is similar to the "read" cases above, except
                // we don't receive the data.
                size_t skip_length{static_cast<size_t>(m_rng.randrange(5))};
                if (currentPos + skip_length > fileSize) continue;
                bf.SetLimit(currentPos + skip_length);
                bf.SkipTo(currentPos + skip_length);
                currentPos += skip_length;
                break;
            }
            case 4: {
                // Find a byte value (that is at or ahead of the current position).
                size_t find = currentPos + m_rng.randrange(8);
                if (find >= fileSize)
                    find = fileSize - 1;
                bf.FindByte(std::byte(find));
                // The value at each offset is the offset.
                CHECK(bf.GetPos() == find);
                currentPos = find;

                bf.SetLimit(currentPos + 1);
                uint8_t i;
                bf >> i;
                CHECK(static_cast<size_t>(i) == currentPos);
                currentPos++;
                break;
            }
            case 5: {
                size_t requestPos = m_rng.randrange(maxPos + 4);
                bool okay = bf.SetPos(requestPos);
                // The new position may differ from the requested position
                // because we may not be able to rewind beyond the rewind
                // window, and we may not be able to move forward beyond the
                // farthest position we've reached so far.
                currentPos = bf.GetPos();
                CHECK(okay == (currentPos == requestPos));
                // Check that we can position within the rewind window.
                if (requestPos <= maxPos &&
                    maxPos > rewindSize &&
                    requestPos >= maxPos - rewindSize) {
                    // We requested a position within the rewind window.
                    CHECK(okay);
                }
                break;
            }
            }
            if (maxPos < currentPos)
                maxPos = currentPos;
        }
        REQUIRE(file.fclose() == 0);
    }
    fs::remove(streams_test_filename);
}

FIXTURE_TEST_CASE(buffered_reader_matches_autofile_random_content, BasicTestingSetup)
{
    const size_t file_size{1 + m_rng.randrange<size_t>(1 << 17)};
    const size_t buf_size{1 + m_rng.randrange(file_size)};
    const FlatFilePos pos{0, 0};

    const FlatFileSeq test_file{m_args.GetDataDirBase(), "buffered_file_test_random", node::BLOCKFILE_CHUNK_SIZE};
    const Obfuscation obfuscation{m_rng.randbytes<Obfuscation::KEY_SIZE>()};

    // Write out the file with random content
    {
        AutoFile f{test_file.Open(pos, /*read_only=*/false), obfuscation};
        f.write(m_rng.randbytes<std::byte>(file_size));
        REQUIRE(f.fclose() == 0);
    }
    CHECK(fs::file_size(test_file.FileName(pos)) == file_size);

    {
        AutoFile direct_file{test_file.Open(pos, /*read_only=*/true), obfuscation};

        AutoFile buffered_file{test_file.Open(pos, /*read_only=*/true), obfuscation};
        BufferedReader buffered_reader{std::move(buffered_file), buf_size};

        for (size_t total_read{0}; total_read < file_size;) {
            const size_t read{Assert(std::min(1 + m_rng.randrange(m_rng.randbool() ? buf_size : 2 * buf_size), file_size - total_read))};

            DataBuffer direct_file_buffer{read};
            direct_file.read(direct_file_buffer);

            DataBuffer buffered_buffer{read};
            buffered_reader.read(buffered_buffer);

            CHECK_EQUAL_RANGES(direct_file_buffer, buffered_buffer);

            total_read += read;
        }

        {
            DataBuffer excess_byte{1};
            CHECK_EXCEPTION(direct_file.read(excess_byte), std::ios_base::failure, HasReason{"end of file"});
        }

        {
            DataBuffer excess_byte{1};
            CHECK_EXCEPTION(buffered_reader.read(excess_byte), std::ios_base::failure, HasReason{"end of file"});
        }
    }

    fs::remove(test_file.FileName(pos));
}

FIXTURE_TEST_CASE(buffered_writer_matches_autofile_random_content, BasicTestingSetup)
{
    const size_t file_size{1 + m_rng.randrange<size_t>(1 << 17)};
    const size_t buf_size{1 + m_rng.randrange(file_size)};
    const FlatFilePos pos{0, 0};

    const FlatFileSeq test_buffered{m_args.GetDataDirBase(), "buffered_write_test", node::BLOCKFILE_CHUNK_SIZE};
    const FlatFileSeq test_direct{m_args.GetDataDirBase(), "direct_write_test", node::BLOCKFILE_CHUNK_SIZE};
    const Obfuscation obfuscation{m_rng.randbytes<Obfuscation::KEY_SIZE>()};

    {
        DataBuffer test_data{m_rng.randbytes<std::byte>(file_size)};

        AutoFile direct_file{test_direct.Open(pos, /*read_only=*/false), obfuscation};

        AutoFile buffered_file{test_buffered.Open(pos, /*read_only=*/false), obfuscation};
        {
            BufferedWriter buffered{buffered_file, buf_size};

            for (size_t total_written{0}; total_written < file_size;) {
                const size_t write_size{Assert(std::min(1 + m_rng.randrange(m_rng.randbool() ? buf_size : 2 * buf_size), file_size - total_written))};

                auto current_span = std::span{test_data}.subspan(total_written, write_size);
                direct_file.write(current_span);
                buffered.write(current_span);

                total_written += write_size;
            }
        }
        REQUIRE(buffered_file.fclose() == 0);
        REQUIRE(direct_file.fclose() == 0);
    }

    // Compare the resulting files
    DataBuffer direct_result{file_size};
    {
        AutoFile verify_direct{test_direct.Open(pos, /*read_only=*/true), obfuscation};
        verify_direct.read(direct_result);

        DataBuffer excess_byte{1};
        CHECK_EXCEPTION(verify_direct.read(excess_byte), std::ios_base::failure, HasReason{"end of file"});
    }

    DataBuffer buffered_result{file_size};
    {
        AutoFile verify_buffered{test_buffered.Open(pos, /*read_only=*/true), obfuscation};
        verify_buffered.read(buffered_result);

        DataBuffer excess_byte{1};
        CHECK_EXCEPTION(verify_buffered.read(excess_byte), std::ios_base::failure, HasReason{"end of file"});
    }

    CHECK_EQUAL_RANGES(direct_result, buffered_result);

    fs::remove(test_direct.FileName(pos));
    fs::remove(test_buffered.FileName(pos));
}

FIXTURE_TEST_CASE(buffered_writer_reader, BasicTestingSetup)
{
    const uint32_t v1{m_rng.rand32()}, v2{m_rng.rand32()}, v3{m_rng.rand32()};
    const fs::path test_file{m_args.GetDataDirBase() / "test_buffered_write_read.bin"};

    // Write out the values through a precisely sized BufferedWriter
    AutoFile file{fsbridge::fopen(test_file, "w+b")};
    {
        BufferedWriter f(file, sizeof(v1) + sizeof(v2) + sizeof(v3));
        f << v1 << v2;
        f.write(std::as_bytes(std::span{&v3, 1}));
    }
    REQUIRE(file.fclose() == 0);

    // Read back and verify using BufferedReader
    {
        uint32_t _v1{0}, _v2{0}, _v3{0};
        AutoFile file{fsbridge::fopen(test_file, "rb")};
        BufferedReader f(std::move(file), sizeof(v1) + sizeof(v2) + sizeof(v3));
        f >> _v1 >> _v2;
        f.read(std::as_writable_bytes(std::span{&_v3, 1}));
        CHECK(_v1 == v1);
        CHECK(_v2 == v2);
        CHECK(_v3 == v3);

        DataBuffer excess_byte{1};
        CHECK_EXCEPTION(f.read(excess_byte), std::ios_base::failure, HasReason{"end of file"});
    }

    fs::remove(test_file);
}

FIXTURE_TEST_CASE(streams_hashed, BasicTestingSetup)
{
    DataStream stream{};
    HashedSourceWriter hash_writer{stream};
    const std::string data{"bitcoin"};
    hash_writer << data;

    HashVerifier hash_verifier{stream};
    std::string result;
    hash_verifier >> result;
    CHECK(data == result);
    CHECK(hash_writer.GetHash() == hash_verifier.GetHash());
}

FIXTURE_TEST_CASE(size_preserves_position, BasicTestingSetup)
{
    const fs::path path = m_args.GetDataDirBase() / "size_pos_test.bin";
    AutoFile f{fsbridge::fopen(path, "w+b")};
    for (uint8_t j = 0; j < 10; ++j) {
        f << j;
    }

    // Test that usage of size() does not change the current position
    //
    // Case: Pos at beginning of the file
    f.seek(0, SEEK_SET);
    (void)f.size();
    uint8_t first{};
    f >> first;
    CHECK(first == 0);

    // Case: Pos at middle of the file
    f.seek(0, SEEK_SET);
    // Move pos to middle
    f.ignore(4);
    (void)f.size();
    uint8_t middle{};
    f >> middle;
    // Pos still at 4
    CHECK(middle == 4);

    // Case: Pos at EOF
    f.seek(0, SEEK_END);
    (void)f.size();
    uint8_t end{};
    CHECK_EXCEPTION(f >> end, std::ios_base::failure, HasReason{"AutoFile::read: end of file"});

    REQUIRE(f.fclose() == 0);
    fs::remove(path);
}

TEST_SUITE_END()
