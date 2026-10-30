#include "axon/protocol/frame.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "axon/protocol/bytes.hpp"

namespace axon::proto {
namespace {

std::array<std::byte, frame_header_len> frame_header(const Message& msg, std::size_t len) {
    std::array<std::byte, frame_header_len> header{};
    store_be(std::span{header}.first<4>(), static_cast<std::uint32_t>(type_of(msg)));
    store_be(std::span{header}.last<4>(), static_cast<std::uint32_t>(len));
    return header;
}

}  // namespace

void send_message(Socket& sock, const Message& msg) {
    const auto payload = encode(msg);
    sock.send_all(frame_header(msg, payload.size()), payload);
}

void send_truncated(Socket& sock, const Message& msg, std::size_t payload_bytes) {
    const auto payload = encode(msg);
    sock.send_all(frame_header(msg, payload.size()),
                  std::span{payload}.first(std::min(payload_bytes, payload.size())));
}

std::optional<FrameHeader> read_frame_header(Socket& sock) {
    std::array<std::byte, frame_header_len> header{};
    if (!sock.recv_all(header)) {
        return std::nullopt;
    }
    return FrameHeader{.type = load_be<std::uint32_t>(std::span{header}.first<4>()),
                       .length = load_be<std::uint32_t>(std::span{header}.last<4>())};
}

std::optional<Message> MessageReader::read(Socket& sock) {
    const auto header = read_frame_header(sock);
    if (!header) {
        return std::nullopt;
    }
    return read_payload(sock, *header);
}

Message MessageReader::read_payload(Socket& sock, const FrameHeader& header) {
    if (header.length > max_payload_) {
        throw DecodeError("frame too large: " + std::to_string(header.length) + " bytes, limit " +
                          std::to_string(max_payload_));
    }
    buf_.resize(header.length);
    if (!sock.recv_all(buf_)) {
        throw ConnectionClosed("connection closed between frame header and payload");
    }
    return decode(header.type, buf_);
}

}  // namespace axon::proto
