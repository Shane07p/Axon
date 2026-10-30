#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "axon/net/socket.hpp"
#include "axon/protocol/messages.hpp"

namespace axon::proto {

// Frame on the wire: type (u32, four ASCII chars) | payload length (u32) |
// payload. Both header fields are big-endian.
inline constexpr std::size_t frame_header_len = 8;
inline constexpr std::size_t default_max_payload = 16 * 1024 * 1024;

// Encodes `msg` and sends it as one frame.
void send_message(Socket& sock, const Message& msg);

// Sends a frame whose header announces all of `msg` but which carries only
// its first `payload_bytes` bytes: a deliberately cut frame, for fault
// injection.
void send_truncated(Socket& sock, const Message& msg, std::size_t payload_bytes);

struct FrameHeader {
    std::uint32_t type = 0;
    std::uint32_t length = 0;  // payload bytes that follow
};

// Reads one frame header, or returns nullopt if the peer closed cleanly
// before it. Throws ConnectionClosed if it closed inside the header. For
// callers that stream a payload themselves (see MessageReader::read_payload).
[[nodiscard]] std::optional<FrameHeader> read_frame_header(Socket& sock);

// Reads frames into a buffer it reuses, so receiving needs no allocation per
// message. Not tied to one socket: keep it across reconnects.
class MessageReader {
public:
    explicit MessageReader(std::size_t max_payload = default_max_payload) noexcept
        : max_payload_{max_payload} {}

    // The next message, or nullopt if the peer closed cleanly between frames.
    // Throws ConnectionClosed if it closed mid-frame, DecodeError for a frame
    // over max_payload (checked before reading it) or a malformed payload.
    // Views in the result (Data::bytes) are valid until the next read().
    [[nodiscard]] std::optional<Message> read(Socket& sock);

    // The rest of read(): the payload of a frame whose header was already read.
    [[nodiscard]] Message read_payload(Socket& sock, const FrameHeader& header);

private:
    std::size_t max_payload_;
    std::vector<std::byte> buf_;
};

}  // namespace axon::proto
