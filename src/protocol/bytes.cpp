#include "axon/protocol/bytes.hpp"

#include <limits>
#include <string>

namespace axon {

void ByteWriter::put_bytes(std::span<const std::byte> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
}

void ByteWriter::put_str(std::string_view s) {
    if (s.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("string too long for message: " + std::to_string(s.size()) +
                                " bytes");
    }
    put_u16(static_cast<std::uint16_t>(s.size()));
    put_bytes(std::as_bytes(std::span{s.data(), s.size()}));
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
