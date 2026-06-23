// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <common/args.h>
#include <flatfile.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(flatfile_tests)

FIXTURE_TEST_CASE(flatfile_filename, BasicTestingSetup)
{
    const auto data_dir = m_args.GetDataDirBase();

    FlatFilePos pos(456, 789);

    FlatFileSeq seq1(data_dir, "a", 16 * 1024);
    CHECK(seq1.FileName(pos) == data_dir / "a00456.dat");

    FlatFileSeq seq2(data_dir / "a", "b", 16 * 1024);
    CHECK(seq2.FileName(pos) == data_dir / "a" / "b00456.dat");

    // Check default constructor IsNull
    assert(FlatFilePos{}.IsNull());
}

FIXTURE_TEST_CASE(flatfile_open, BasicTestingSetup)
{
    const auto data_dir = m_args.GetDataDirBase();
    FlatFileSeq seq(data_dir, "a", 16 * 1024);

    std::string line1("A purely peer-to-peer version of electronic cash would allow online "
                      "payments to be sent directly from one party to another without going "
                      "through a financial institution.");
    std::string line2("Digital signatures provide part of the solution, but the main benefits are "
                      "lost if a trusted third party is still required to prevent double-spending.");

    uint64_t pos1{0};
    uint64_t pos2{pos1 + GetSerializeSize(line1)};

    // Write first line to file.
    {
        AutoFile file{seq.Open(FlatFilePos(0, pos1))};
        file << LIMITED_STRING(line1, 256);
        REQUIRE(file.fclose() == 0);
    }

    // Attempt to append to file opened in read-only mode.
    {
        AutoFile file{seq.Open(FlatFilePos(0, pos2), true)};
        CHECK_THROWS_AS(file << LIMITED_STRING(line2, 256), std::ios_base::failure);
    }

    // Append second line to file.
    {
        AutoFile file{seq.Open(FlatFilePos(0, pos2))};
        file << LIMITED_STRING(line2, 256);
        REQUIRE(file.fclose() == 0);
    }

    // Read text from file in read-only mode.
    {
        std::string text;
        AutoFile file{seq.Open(FlatFilePos(0, pos1), true)};

        file >> LIMITED_STRING(text, 256);
        CHECK(text == line1);

        file >> LIMITED_STRING(text, 256);
        CHECK(text == line2);
    }

    // Read text from file with position offset.
    {
        std::string text;
        AutoFile file{seq.Open(FlatFilePos(0, pos2))};

        file >> LIMITED_STRING(text, 256);
        CHECK(text == line2);
        REQUIRE(file.fclose() == 0);
    }

    // Ensure another file in the sequence has no data.
    {
        std::string text;
        AutoFile file{seq.Open(FlatFilePos(1, pos2))};
        CHECK_THROWS_AS(file >> LIMITED_STRING(text, 256), std::ios_base::failure);
        REQUIRE(file.fclose() == 0);
    }
}

FIXTURE_TEST_CASE(flatfile_allocate, BasicTestingSetup)
{
    const auto data_dir = m_args.GetDataDirBase();
    FlatFileSeq seq(data_dir, "a", 100);

    bool out_of_space;

    CHECK(seq.Allocate(FlatFilePos(0, 0), 1, out_of_space) == 100U);
    CHECK(fs::file_size(seq.FileName(FlatFilePos(0, 0))) == 100U);
    CHECK(!out_of_space);

    CHECK(seq.Allocate(FlatFilePos(0, 99), 1, out_of_space) == 0U);
    CHECK(fs::file_size(seq.FileName(FlatFilePos(0, 99))) == 100U);
    CHECK(!out_of_space);

    CHECK(seq.Allocate(FlatFilePos(0, 99), 2, out_of_space) == 101U);
    CHECK(fs::file_size(seq.FileName(FlatFilePos(0, 99))) == 200U);
    CHECK(!out_of_space);
}

FIXTURE_TEST_CASE(flatfile_flush, BasicTestingSetup)
{
    const auto data_dir = m_args.GetDataDirBase();
    FlatFileSeq seq(data_dir, "a", 100);

    bool out_of_space;
    seq.Allocate(FlatFilePos(0, 0), 1, out_of_space);

    // Flush without finalize should not truncate file.
    seq.Flush(FlatFilePos(0, 1));
    CHECK(fs::file_size(seq.FileName(FlatFilePos(0, 1))) == 100U);

    // Flush with finalize should truncate file.
    seq.Flush(FlatFilePos(0, 1), true);
    CHECK(fs::file_size(seq.FileName(FlatFilePos(0, 1))) == 1U);
}

TEST_SUITE_END()
