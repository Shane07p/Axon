#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axon/protocol/bytes.hpp"

namespace {

using axon::ByteReader;
using axon::ByteWriter;
using axon::DecodeError;

// Big-endian conversion is constexpr, so it can be checked at compile time.
constexpr auto be32 = [] {
    std::array<std::byte, 4> a{};
    axon::store_be<std::uint32_t>(a, 0x01020304);
    return a;
}();
static_assert(be32[0] == std::byte{0x01} && be32[3] == std::byte{0x04});
static_assert(axon::load_be<std::uint32_t>(be32) == 0x01020304);

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

bool same(std::span<const std::byte> a, std::span<const std::byte> b) {
    return std::ranges::equal(a, b);
}

TEST(ByteWriter, IntegersAreBigEndian) {
    ByteWriter w;
    w.put_u8(0xAB);
    w.put_u16(0x0102);
    w.put_u32(0x03040506);
    w.put_u64(0x0708090A0B0C0D0E);
    EXPECT_TRUE(same(w.view(), bytes_of({0xAB, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                         0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E})));
}

TEST(ByteWriter, StringHasLengthPrefix) {
    ByteWriter w;
    w.put_str("hi");
    EXPECT_TRUE(same(w.view(), bytes_of({0x00, 0x02, 'h', 'i'})));
}

TEST(ByteWriter, RejectsStringLongerThanU16) {
    ByteWriter w;
    const std::string too_long(std::size_t{std::numeric_limits<std::uint16_t>::max()} + 1, 'x');
    EXPECT_THROW(w.put_str(too_long), std::length_error);
    EXPECT_EQ(w.size(), 0u);
}

TEST(ByteWriter, PutStrMayCopyFromItsOwnBuffer) {
    ByteWriter w;
    w.put_str("abc");
    for (int i = 0; i < 8; ++i) {  // the buffer grows (and moves) while copying
        ByteReader r{w.view()};
        w.put_str(r.get_str());
    }
    ByteReader r{w.view()};
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(r.get_str(), "abc");
    }
    EXPECT_NO_THROW(r.expect_end());
}

TEST(ByteWriter, PutBytesMayCopyFromItsOwnBuffer) {
    ByteWriter w;
    w.put_u32(0x01020304);
    for (int i = 0; i < 6; ++i) {
        w.put_bytes(w.view());  // doubles: 4 -> 256 bytes
    }
    ASSERT_EQ(w.size(), 256u);
    ByteReader r{w.view()};
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(r.get_u32(), 0x01020304u);
    }
}

TEST(ByteWriter, TakeMovesTheBytesOut) {
    ByteWriter w;
    w.put_u32(42);
    const auto bytes = std::move(w).take();
    EXPECT_EQ(bytes.size(), 4u);
}

TEST(Bytes, RoundTripsEveryType) {
    const std::string longest(std::numeric_limits<std::uint16_t>::max(), 'z');
    const std::string_view with_nul{"a\0b", 3};
    const auto raw = bytes_of({0xDE, 0xAD, 0xBE, 0xEF});

    ByteWriter w;
    w.put_u8(0);
    w.put_u8(0xFF);
    w.put_u16(0xFFFF);
    w.put_u32(0xFFFFFFFF);
    w.put_u64(0);
    w.put_u64(std::numeric_limits<std::uint64_t>::max());
    w.put_str("");
    w.put_str(with_nul);
    w.put_str("caf\xC3\xA9");  // UTF-8 "café"
    w.put_str(longest);
    w.put_bytes(raw);

    ByteReader r{w.view()};
    EXPECT_EQ(r.get_u8(), 0);
    EXPECT_EQ(r.get_u8(), 0xFF);
    EXPECT_EQ(r.get_u16(), 0xFFFF);
    EXPECT_EQ(r.get_u32(), 0xFFFFFFFFu);
    EXPECT_EQ(r.get_u64(), 0u);
    EXPECT_EQ(r.get_u64(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(r.get_str(), "");
    EXPECT_EQ(r.get_str(), with_nul);
    EXPECT_EQ(r.get_str(), "caf\xC3\xA9");
    EXPECT_EQ(r.get_str(), longest);
    EXPECT_TRUE(same(r.get_bytes(raw.size()), raw));
    EXPECT_NO_THROW(r.expect_end());
}

TEST(ByteReader, ThrowsWhenTruncated) {
    const auto data = bytes_of({0x00, 0x01, 0x02});
    ByteReader r{data};
    EXPECT_THROW((void)r.get_u32(), DecodeError);
    EXPECT_EQ(r.remaining(), 3u);  // a failed read consumes nothing
    EXPECT_EQ(r.get_u16(), 0x0001);
}

TEST(ByteReader, ThrowsWhenStringLengthOverruns) {
    const auto data = bytes_of({0x00, 0x0A, 'a', 'b', 'c'});  // claims 10 bytes, has 3
    ByteReader r{data};
    EXPECT_THROW((void)r.get_str(), DecodeError);
}

TEST(ByteReader, ExpectEndRejectsTrailingBytes) {
    const auto data = bytes_of({0x00, 0x07, 0xFF});
    ByteReader r{data};
    EXPECT_EQ(r.get_u16(), 7);
    EXPECT_THROW(r.expect_end(), DecodeError);
}

TEST(ByteReader, EveryTruncatedPrefixIsRejected) {
    ByteWriter w;
    w.put_u16(1);
    w.put_str("topic");
    w.put_u64(123456789);
    const auto full = w.view();

    for (std::size_t len = 0; len < full.size(); ++len) {
        ByteReader r{full.first(len)};
        EXPECT_THROW(
            {
                (void)r.get_u16();
                (void)r.get_str();
                (void)r.get_u64();
            },
            DecodeError)
            << "prefix of " << len << " bytes was accepted";
    }
}

}  // namespace
