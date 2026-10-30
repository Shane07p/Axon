#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace axon {

namespace detail {

constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

inline constexpr auto crc32_table = make_crc32_table();  // built at compile time

}  // namespace detail

// CRC-32 (IEEE 802.3, the one zlib, gzip and PNG use). Start from 0 and feed
// the data in any number of pieces: crc = crc32_update(crc, piece).
[[nodiscard]] constexpr std::uint32_t crc32_update(std::uint32_t crc,
                                                   std::span<const std::byte> data) noexcept {
    crc = ~crc;
    for (const std::byte b : data) {
        crc = detail::crc32_table[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

}  // namespace axon
