#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "axon/crc32.hpp"

namespace {

using axon::crc32_update;

template <std::size_t N>
constexpr std::array<std::byte, N - 1> ascii(const char (&s)[N]) {
    std::array<std::byte, N - 1> out{};
    for (std::size_t i = 0; i + 1 < N; ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

// The standard CRC-32 check value, verified at compile time.
static_assert(crc32_update(0, ascii("123456789")) == 0xCBF43926);
static_assert(crc32_update(0, {}) == 0);

std::uint32_t crc_of(std::string_view s) {
    return crc32_update(0, std::as_bytes(std::span{s.data(), s.size()}));
}

TEST(Crc32, KnownValues) {
    EXPECT_EQ(crc_of("a"), 0xE8B7BE43u);
    EXPECT_EQ(crc_of("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(Crc32, PiecesGiveTheSameResultAsOneCall) {
    const std::string_view text = "The quick brown fox jumps over the lazy dog";
    const auto whole = crc_of(text);
    for (std::size_t cut = 0; cut <= text.size(); ++cut) {
        std::uint32_t crc = 0;
        crc = crc32_update(crc, std::as_bytes(std::span{text.data(), cut}));
        crc = crc32_update(crc, std::as_bytes(std::span{text.data() + cut, text.size() - cut}));
        EXPECT_EQ(crc, whole) << "split at " << cut;
    }
}

}  // namespace
