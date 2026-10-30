#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace axon {

template <typename T>
concept WireInt = std::unsigned_integral<T> && !std::same_as<T, bool>;

// Writes `v` to `out` in big-endian (network) byte order.
template <WireInt T>
constexpr void store_be(std::span<std::byte, sizeof(T)> out, T v) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out[i] = static_cast<std::byte>(v >> (8 * (sizeof(T) - 1 - i)));
    }
}

// Reads a big-endian (network byte order) value from `in`.
template <WireInt T>
constexpr T load_be(std::span<const std::byte, sizeof(T)> in) noexcept {
    T v = 0;
    for (const std::byte b : in) {
        v = static_cast<T>((v << 8) | std::to_integer<T>(b));
    }
    return v;
}

// A malformed or truncated message.
class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Builds a message payload. Integers are big-endian; strings carry a u16
// length prefix.
class ByteWriter {
public:
    void put_u8(std::uint8_t v) { put(v); }
    void put_u16(std::uint16_t v) { put(v); }
    void put_u32(std::uint32_t v) { put(v); }
    void put_u64(std::uint64_t v) { put(v); }
    void put_bytes(std::span<const std::byte> data);

    // Throws std::length_error if `s` is longer than 65535 bytes.
    void put_str(std::string_view s);

    [[nodiscard]] std::span<const std::byte> view() const noexcept { return buf_; }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] std::vector<std::byte> take() && noexcept { return std::move(buf_); }

private:
    template <WireInt T>
    void put(T v) {
        buf_.resize(buf_.size() + sizeof(T));
        store_be(std::span{buf_}.last<sizeof(T)>(), v);
    }

    std::vector<std::byte> buf_;
};

// Reads a message payload written by ByteWriter, checking every bound.
// Reading past the end throws DecodeError. Does not own the bytes: views
// returned by get_bytes() and get_str() point into them.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_{data} {}

    [[nodiscard]] std::uint8_t get_u8() { return get<std::uint8_t>(); }
    [[nodiscard]] std::uint16_t get_u16() { return get<std::uint16_t>(); }
    [[nodiscard]] std::uint32_t get_u32() { return get<std::uint32_t>(); }
    [[nodiscard]] std::uint64_t get_u64() { return get<std::uint64_t>(); }
    [[nodiscard]] std::span<const std::byte> get_bytes(std::size_t n);
    [[nodiscard]] std::string_view get_str();

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

    // Throws DecodeError if any bytes are left unread.
    void expect_end() const;

private:
    template <WireInt T>
    T get() {
        return load_be<T>(get_bytes(sizeof(T)).first<sizeof(T)>());
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

}  // namespace axon
