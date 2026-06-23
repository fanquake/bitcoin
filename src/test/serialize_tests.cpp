// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hash.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(serialize_tests)

// For testing move-semantics, declare a version of datastream that can be moved
// but is not copyable.
class UncopyableStream : public DataStream
{
public:
    using DataStream::DataStream;
    UncopyableStream(const UncopyableStream&) = delete;
    UncopyableStream& operator=(const UncopyableStream&) = delete;
    UncopyableStream(UncopyableStream&&) noexcept = default;
    UncopyableStream& operator=(UncopyableStream&&) noexcept = default;
};

class CSerializeMethodsTestSingle
{
protected:
    int intval;
    bool boolval;
    std::string stringval;
    char charstrval[16];
    CTransactionRef txval;
public:
    CSerializeMethodsTestSingle() = default;
    CSerializeMethodsTestSingle(int intvalin, bool boolvalin, std::string stringvalin, const uint8_t* charstrvalin, const CTransactionRef& txvalin) : intval(intvalin), boolval(boolvalin), stringval(std::move(stringvalin)), txval(txvalin)
    {
        memcpy(charstrval, charstrvalin, sizeof(charstrval));
    }

    SERIALIZE_METHODS(CSerializeMethodsTestSingle, obj)
    {
        READWRITE(obj.intval);
        READWRITE(obj.boolval);
        READWRITE(obj.stringval);
        READWRITE(obj.charstrval);
        READWRITE(TX_WITH_WITNESS(obj.txval));
    }

    bool operator==(const CSerializeMethodsTestSingle& rhs) const
    {
        return intval == rhs.intval &&
               boolval == rhs.boolval &&
               stringval == rhs.stringval &&
               strcmp(charstrval, rhs.charstrval) == 0 &&
               *txval == *rhs.txval;
    }
};

class CSerializeMethodsTestMany : public CSerializeMethodsTestSingle
{
public:
    using CSerializeMethodsTestSingle::CSerializeMethodsTestSingle;

    SERIALIZE_METHODS(CSerializeMethodsTestMany, obj)
    {
        READWRITE(obj.intval, obj.boolval, obj.stringval, obj.charstrval, TX_WITH_WITNESS(obj.txval));
    }
};

FIXTURE_TEST_CASE(sizes, BasicTestingSetup)
{
    CHECK(sizeof(unsigned char) == GetSerializeSize((unsigned char)0));
    CHECK(sizeof(int8_t) == GetSerializeSize(int8_t(0)));
    CHECK(sizeof(uint8_t) == GetSerializeSize(uint8_t(0)));
    CHECK(sizeof(int16_t) == GetSerializeSize(int16_t(0)));
    CHECK(sizeof(uint16_t) == GetSerializeSize(uint16_t(0)));
    CHECK(sizeof(int32_t) == GetSerializeSize(int32_t(0)));
    CHECK(sizeof(uint32_t) == GetSerializeSize(uint32_t(0)));
    CHECK(sizeof(int64_t) == GetSerializeSize(int64_t(0)));
    CHECK(sizeof(uint64_t) == GetSerializeSize(uint64_t(0)));
    // Bool is serialized as uint8_t
    CHECK(sizeof(uint8_t) == GetSerializeSize(bool(0)));

    // Sanity-check GetSerializeSize and c++ type matching
    CHECK(GetSerializeSize((unsigned char)0) == 1U);
    CHECK(GetSerializeSize(int8_t(0)) == 1U);
    CHECK(GetSerializeSize(uint8_t(0)) == 1U);
    CHECK(GetSerializeSize(int16_t(0)) == 2U);
    CHECK(GetSerializeSize(uint16_t(0)) == 2U);
    CHECK(GetSerializeSize(int32_t(0)) == 4U);
    CHECK(GetSerializeSize(uint32_t(0)) == 4U);
    CHECK(GetSerializeSize(int64_t(0)) == 8U);
    CHECK(GetSerializeSize(uint64_t(0)) == 8U);
    CHECK(GetSerializeSize(bool(0)) == 1U);
    CHECK(GetSerializeSize(std::array<uint8_t, 1>{0}) == 1U);
    CHECK(GetSerializeSize(std::array<uint8_t, 2>{0, 0}) == 2U);
}

FIXTURE_TEST_CASE(varints, BasicTestingSetup)
{
    // encode

    DataStream ss{};
    DataStream::size_type size = 0;
    for (int i = 0; i < 100000; i++) {
        ss << VARINT_MODE(i, VarIntMode::NONNEGATIVE_SIGNED);
        size += ::GetSerializeSize(VARINT_MODE(i, VarIntMode::NONNEGATIVE_SIGNED));
        CHECK(size == ss.size());
    }

    for (uint64_t i = 0;  i < 100000000000ULL; i += 999999937) {
        ss << VARINT(i);
        size += ::GetSerializeSize(VARINT(i));
        CHECK(size == ss.size());
    }

    // decode
    for (int i = 0; i < 100000; i++) {
        int j = -1;
        ss >> VARINT_MODE(j, VarIntMode::NONNEGATIVE_SIGNED);
        CHECK(i == j, "decoded:" << j << " expected:" << i);
    }

    for (uint64_t i = 0;  i < 100000000000ULL; i += 999999937) {
        uint64_t j = std::numeric_limits<uint64_t>::max();
        ss >> VARINT(j);
        CHECK(i == j, "decoded:" << j << " expected:" << i);
    }
}

FIXTURE_TEST_CASE(varints_bitpatterns, BasicTestingSetup)
{
    DataStream ss{};
    ss << VARINT_MODE(0, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "00"); ss.clear();
    ss << VARINT_MODE(0x7f, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "7f"); ss.clear();
    ss << VARINT_MODE(int8_t{0x7f}, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "7f"); ss.clear();
    ss << VARINT_MODE(0x80, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "8000"); ss.clear();
    ss << VARINT(uint8_t{0x80}); CHECK(HexStr(ss) == "8000"); ss.clear();
    ss << VARINT_MODE(0x1234, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "a334"); ss.clear();
    ss << VARINT_MODE(int16_t{0x1234}, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "a334"); ss.clear();
    ss << VARINT_MODE(0xffff, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "82fe7f"); ss.clear();
    ss << VARINT(uint16_t{0xffff}); CHECK(HexStr(ss) == "82fe7f"); ss.clear();
    ss << VARINT_MODE(0x123456, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "c7e756"); ss.clear();
    ss << VARINT_MODE(int32_t{0x123456}, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "c7e756"); ss.clear();
    ss << VARINT(0x80123456U); CHECK(HexStr(ss) == "86ffc7e756"); ss.clear();
    ss << VARINT(uint32_t{0x80123456U}); CHECK(HexStr(ss) == "86ffc7e756"); ss.clear();
    ss << VARINT(0xffffffff); CHECK(HexStr(ss) == "8efefefe7f"); ss.clear();
    ss << VARINT_MODE(0x7fffffffffffffffLL, VarIntMode::NONNEGATIVE_SIGNED); CHECK(HexStr(ss) == "fefefefefefefefe7f"); ss.clear();
    ss << VARINT(0xffffffffffffffffULL); CHECK(HexStr(ss) == "80fefefefefefefefe7f"); ss.clear();
}

FIXTURE_TEST_CASE(compactsize, BasicTestingSetup)
{
    DataStream ss{};
    std::vector<char>::size_type i, j;

    for (i = 1; i <= MAX_SIZE; i *= 2)
    {
        WriteCompactSize(ss, i-1);
        WriteCompactSize(ss, i);
    }
    for (i = 1; i <= MAX_SIZE; i *= 2)
    {
        j = ReadCompactSize(ss);
        CHECK((i-1) == j, "decoded:" << j << " expected:" << (i-1));
        j = ReadCompactSize(ss);
        CHECK(i == j, "decoded:" << j << " expected:" << i);
    }
}

static bool isCanonicalException(const std::ios_base::failure& ex)
{
    std::ios_base::failure expectedException("non-canonical ReadCompactSize()");

    // The string returned by what() can be different for different platforms.
    // Instead of directly comparing the ex.what() with an expected string,
    // create an instance of exception to see if ex.what() matches
    // the expected explanatory string returned by the exception instance.
    return strcmp(expectedException.what(), ex.what()) == 0;
}

FIXTURE_TEST_CASE(vector_bool, BasicTestingSetup)
{
    std::vector<uint8_t> vec1{1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1};
    std::vector<bool> vec2{1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1};

    CHECK(vec1 == std::vector<uint8_t>(vec2.begin(), vec2.end()));
    CHECK((HashWriter{} << vec1).GetHash() == (HashWriter{} << vec2).GetHash());
}

FIXTURE_TEST_CASE(array, BasicTestingSetup)
{
    std::array<uint8_t, 32> array1{1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1};
    DataStream ds;
    ds << array1;
    std::array<uint8_t, 32> array2;
    ds >> array2;
    CHECK(array1 == array2);
}

FIXTURE_TEST_CASE(noncanonical, BasicTestingSetup)
{
    // Write some non-canonical CompactSize encodings, and
    // make sure an exception is thrown when read back.
    DataStream ss{};
    std::vector<char>::size_type n;

    // zero encoded with three bytes:
    ss << std::span{"\xfd\x00\x00"}.first(3);
    CHECK_EXCEPTION(ReadCompactSize(ss), std::ios_base::failure, isCanonicalException);

    // 0xfc encoded with three bytes:
    ss << std::span{"\xfd\xfc\x00"}.first(3);
    CHECK_EXCEPTION(ReadCompactSize(ss), std::ios_base::failure, isCanonicalException);

    // 0xfd encoded with three bytes is OK:
    ss << std::span{"\xfd\xfd\x00"}.first(3);
    n = ReadCompactSize(ss);
    CHECK(n == 0xfdU);

    // zero encoded with five bytes:
    ss << std::span{"\xfe\x00\x00\x00\x00"}.first(5);
    CHECK_EXCEPTION(ReadCompactSize(ss), std::ios_base::failure, isCanonicalException);

    // 0xffff encoded with five bytes:
    ss << std::span{"\xfe\xff\xff\x00\x00"}.first(5);
    CHECK_EXCEPTION(ReadCompactSize(ss), std::ios_base::failure, isCanonicalException);

    // zero encoded with nine bytes:
    ss << std::span{"\xff\x00\x00\x00\x00\x00\x00\x00\x00"}.first(9);
    CHECK_EXCEPTION(ReadCompactSize(ss), std::ios_base::failure, isCanonicalException);

    // 0x01ffffff encoded with nine bytes:
    ss << std::span{"\xff\xff\xff\xff\x01\x00\x00\x00\x00"}.first(9);
    CHECK_EXCEPTION(ReadCompactSize(ss), std::ios_base::failure, isCanonicalException);
}

FIXTURE_TEST_CASE(string_view, BasicTestingSetup)
{
    const std::string_view sv{"hello, world"};
    DataStream ss;
    ss << sv;
    std::string s;
    ss >> s;
    CHECK(sv == s);
}

FIXTURE_TEST_CASE(limited_vector, BasicTestingSetup)
{
    const std::vector<int> v = {1,2,3,4,-5,-6,-7,-8,-9,-10,10000,20000,-30000};

    auto check = [&]<size_t N>() {
        DataStream ss;
        ss << v;
        try {
            std::vector<int> r;
            ss >> LIMITED_VECTOR(r, N);
            CHECK(r.size() <= N);
            CHECK(std::ranges::equal(r, v));
        } catch (const std::ios_base::failure&) {
            CHECK(v.size() > N);
        }
    };
    check.operator()<0>();
    check.operator()<10>();
    check.operator()<12>();
    check.operator()<13>();
    check.operator()<14>();
    check.operator()<100>();
}

FIXTURE_TEST_CASE(class_methods, BasicTestingSetup)
{
    int intval(100);
    bool boolval(true);
    std::string stringval("testing");
    const uint8_t charstrval[16]{"testing charstr"};
    CMutableTransaction txval;
    CTransactionRef tx_ref{MakeTransactionRef(txval)};
    CSerializeMethodsTestSingle methodtest1(intval, boolval, stringval, charstrval, tx_ref);
    CSerializeMethodsTestMany methodtest2(intval, boolval, stringval, charstrval, tx_ref);
    CSerializeMethodsTestSingle methodtest3;
    CSerializeMethodsTestMany methodtest4;
    DataStream ss;
    CHECK(methodtest1 == methodtest2);
    ss << methodtest1;
    ss >> methodtest4;
    ss << methodtest2;
    ss >> methodtest3;
    CHECK(methodtest1 == methodtest2);
    CHECK(methodtest2 == methodtest3);
    CHECK(methodtest3 == methodtest4);

    DataStream ss2;
    ss2 << intval << boolval << stringval << charstrval << TX_WITH_WITNESS(txval);
    ss2 >> methodtest3;
    CHECK(methodtest3 == methodtest4);
    {
        DataStream ds;
        const std::string in{"ab"};
        ds << std::span{in} << std::byte{'c'};
        std::array<std::byte, 2> out;
        std::byte out_3;
        ds >> std::span{out} >> out_3;
        CHECK(out.at(0) == std::byte{'a'});
        CHECK(out.at(1) == std::byte{'b'});
        CHECK(out_3 == std::byte{'c'});
    }
}

struct BaseFormat {
    const enum {
        RAW,
        HEX,
    } m_base_format;
    SER_PARAMS_OPFUNC
};
constexpr BaseFormat RAW{BaseFormat::RAW};
constexpr BaseFormat HEX{BaseFormat::HEX};

/// (Un)serialize a number as raw byte or 2 hexadecimal chars.
class Base
{
public:
    uint8_t m_base_data;

    Base() : m_base_data(17) {}
    explicit Base(uint8_t data) : m_base_data(data) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (s.template GetParams<BaseFormat>().m_base_format == BaseFormat::RAW) {
            s << m_base_data;
        } else {
            s << std::span<const char>{HexStr(std::span{&m_base_data, 1})};
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (s.template GetParams<BaseFormat>().m_base_format == BaseFormat::RAW) {
            s >> m_base_data;
        } else {
            std::string hex{"aa"};
            s >> std::span{hex}.first(hex.size());
            m_base_data = TryParseHex<uint8_t>(hex).value().at(0);
        }
    }
};

class DerivedAndBaseFormat
{
public:
    BaseFormat m_base_format;

    enum class DerivedFormat {
        LOWER,
        UPPER,
    } m_derived_format;

    SER_PARAMS_OPFUNC
};

class Derived : public Base
{
public:
    std::string m_derived_data;

    SERIALIZE_METHODS(Derived, obj)
    {
        auto& fmt = SER_PARAMS(DerivedAndBaseFormat);
        READWRITE(fmt.m_base_format(AsBase<Base>(obj)));

        if (ser_action.ForRead()) {
            std::string str;
            s >> str;
            SER_READ(obj, obj.m_derived_data = str);
        } else {
            s << (fmt.m_derived_format == DerivedAndBaseFormat::DerivedFormat::LOWER ?
                      ToLower(obj.m_derived_data) :
                      ToUpper(obj.m_derived_data));
        }
    }
};

struct OtherParam {
    uint8_t param;
    SER_PARAMS_OPFUNC
};

//! Checker for value of OtherParam. When being serialized, serializes the
//! param to the stream. When being unserialized, verifies the value in the
//! stream matches the param.
class OtherParamChecker
{
public:
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        const uint8_t param = s.template GetParams<OtherParam>().param;
        s << param;
    }

    template <typename Stream>
    void Unserialize(Stream& s) const
    {
        const uint8_t param = s.template GetParams<OtherParam>().param;
        uint8_t value;
        s >> value;
        CHECK(value == param);
    }
};

//! Test creating a stream with multiple parameters and making sure
//! serialization code requiring different parameters can retrieve them. Also
//! test that earlier parameters take precedence if the same parameter type is
//! specified twice. (Choice of whether earlier or later values take precedence
//! or multiple values of the same type are allowed was arbitrary, and just
//! decided based on what would require smallest amount of ugly C++ template
//! code. Intent of the test is to just ensure there is no unexpected behavior.)
FIXTURE_TEST_CASE(with_params_multi, BasicTestingSetup)
{
    const OtherParam other_param_used{.param = 0x10};
    const OtherParam other_param_ignored{.param = 0x11};
    const OtherParam other_param_override{.param = 0x12};
    const OtherParamChecker check;
    DataStream stream;
    ParamsStream pstream{stream, RAW, other_param_used, other_param_ignored};

    Base base1{0x20};
    pstream << base1 << check << other_param_override(check);
    CHECK(stream.str() == "\x20\x10\x12");

    Base base2;
    pstream >> base2 >> check >> other_param_override(check);
    CHECK(base2.m_base_data == 0x20);
}

//! Test creating a ParamsStream that moves from a stream argument.
FIXTURE_TEST_CASE(with_params_move, BasicTestingSetup)
{
    UncopyableStream stream{MakeByteSpan(std::string_view{"abc"})};
    ParamsStream pstream{std::move(stream), RAW, HEX, RAW};
    CHECK(pstream.GetStream().str() == "abc");
    pstream.GetStream().clear();

    Base base1{0x20};
    pstream << base1;
    CHECK(pstream.GetStream().str() == "\x20");

    Base base2;
    pstream >> base2;
    CHECK(base2.m_base_data == 0x20);
}

FIXTURE_TEST_CASE(with_params_base, BasicTestingSetup)
{
    Base b{0x0F};

    DataStream stream;

    stream << RAW(b);
    CHECK(stream.str() == "\x0F");

    b.m_base_data = 0;
    stream >> RAW(b);
    CHECK(b.m_base_data == 0x0F);

    stream.clear();

    stream << HEX(b);
    CHECK(stream.str() == "0f");

    b.m_base_data = 0;
    stream >> HEX(b);
    CHECK(b.m_base_data == 0x0F);
}

FIXTURE_TEST_CASE(with_params_vector_of_base, BasicTestingSetup)
{
    std::vector<Base> v{Base{0x0F}, Base{0xFF}};

    DataStream stream;

    stream << RAW(v);
    CHECK(stream.str() == "\x02\x0F\xFF");

    v[0].m_base_data = 0;
    v[1].m_base_data = 0;
    stream >> RAW(v);
    CHECK(v[0].m_base_data == 0x0F);
    CHECK(v[1].m_base_data == 0xFF);

    stream.clear();

    stream << HEX(v);
    CHECK(stream.str() == "\x02"
                                    "0fff");

    v[0].m_base_data = 0;
    v[1].m_base_data = 0;
    stream >> HEX(v);
    CHECK(v[0].m_base_data == 0x0F);
    CHECK(v[1].m_base_data == 0xFF);
}

constexpr DerivedAndBaseFormat RAW_LOWER{{BaseFormat::RAW}, DerivedAndBaseFormat::DerivedFormat::LOWER};
constexpr DerivedAndBaseFormat HEX_UPPER{{BaseFormat::HEX}, DerivedAndBaseFormat::DerivedFormat::UPPER};

FIXTURE_TEST_CASE(with_params_derived, BasicTestingSetup)
{
    Derived d;
    d.m_base_data = 0x0F;
    d.m_derived_data = "xY";

    DataStream stream;

    stream << RAW_LOWER(d);

    stream << HEX_UPPER(d);

    CHECK(stream.str() == "\x0F\x02xy"
                                    "0f\x02XY");
}

TEST_SUITE_END()
