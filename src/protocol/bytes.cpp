#include "axon/protocol/bytes.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <string>

namespace axon {

void ByteWriter::put_bytes(std::span<const std::byte> data) { append({}, data); }

void ByteWriter::put_str(std::string_view s) {
    if (s.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("string too long for message: " + std::to_string(s.size()) +
                                " bytes");
    }
    std::array<std::byte, 2> len{};
    store_be(std::span{len}, static_cast<std::uint16_t>(s.size()));
    append(len, std::as_bytes(std::span{s.data(), s.size()}));
}

void ByteWriter::append(std::span<const std::byte> head, std::span<const std::byte> data) {
    // `data` may be a view into buf_ itself (e.g. from a ByteReader over this
    // writer). Growing can move the buffer, so remember where it was first.
    const std::byte* const old_base = buf_.data();
    const bool aliased = std::less_equal<>{}(old_base, data.data()) &&
                         std::less<>{}(data.data(), old_base + buf_.size());
    const auto offset = aliased ? static_cast<std::size_t>(data.data() - old_base) : 0;

    const std::size_t at = buf_.size();
    buf_.resize(at + head.size() + data.size());
    const std::span<const std::byte> src =
        aliased ? std::span{buf_}.subspan(offset, data.size()) : data;
    std::byte* const out = buf_.data() + at;
    std::ranges::copy(head, out);
    std::ranges::copy(src, out + head.size());
}

std::span<const std::byte> ByteReader::get_bytes(std::size_t n) {
    if (n > remaining()) {
        throw DecodeError("truncated message: need " + std::to_string(n) + " bytes, " +
                          std::to_string(remaining()) + " left");
    }
    const auto out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
}

std::string_view ByteReader::get_str() {
    const auto bytes = get_bytes(get_u16());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void ByteReader::expect_end() const {
    if (remaining() != 0) {
        throw DecodeError("malformed message: " + std::to_string(remaining()) +
                          " unexpected trailing bytes");
    }
}

}  // namespace axon
