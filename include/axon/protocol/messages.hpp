#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace axon::proto {

inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::size_t max_topic_len = 255;
inline constexpr std::size_t max_name_len = 255;
inline constexpr std::size_t max_req_ranges = 16;
inline constexpr std::size_t max_data_len = 64 * 1024;

// Message type on the wire: four ASCII characters, readable in a hex dump.
constexpr std::uint32_t tag(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

enum class MsgType : std::uint32_t {
    Get = tag("GET "),
    Ok = tag("OK  "),
    Err = tag("ERR "),
    Req = tag("REQ "),
    Data = tag("DATA"),
    Ack = tag("ACK "),
};

// One file of a topic. `crc` is the CRC-32 of its content.
struct FileEntry {
    std::string name{};
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    bool operator==(const FileEntry&) const = default;
};

// C->S, first message on every connection. session_id 0 asks for a new
// session. A Get from another protocol version decodes to just its version,
// so the server can still answer Err{BadVersion}.
struct Get {
    static constexpr MsgType type = MsgType::Get;
    std::uint16_t version = protocol_version;
    std::string topic{};
    std::uint64_t session_id = 0;
    bool want_list = false;
    bool operator==(const Get&) const = default;
};

// S->C, accepts a Get. `files` is present for a new session or when the
// client asked for the list; file ids are indexes into it.
struct Ok {
    static constexpr MsgType type = MsgType::Ok;
    std::uint64_t session_id = 0;
    std::uint64_t dataset_id = 0;
    std::optional<std::vector<FileEntry>> files{};
    bool operator==(const Ok&) const = default;
};

enum class ErrCode : std::uint16_t {
    BadRequest = 1,
    BadVersion = 2,
    UnknownTopic = 3,
    SessionNotFound = 4,
    TopicMismatch = 5,
    BadRange = 6,
    Internal = 7,
};

// Either direction: the request failed and the connection will close.
struct Err {
    static constexpr MsgType type = MsgType::Err;
    ErrCode code = ErrCode::Internal;
    std::string reason{};
    bool operator==(const Err&) const = default;
};

struct Range {
    std::uint32_t file_id = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool operator==(const Range&) const = default;
};

// C->S: send these byte ranges (1 to max_req_ranges of them).
struct Req {
    static constexpr MsgType type = MsgType::Req;
    std::vector<Range> ranges{};
    bool operator==(const Req&) const = default;
};

// S->C: `bytes` of file `file_id`, starting at `offset`. A decoded Data
// views the payload it was decoded from rather than copying it.
struct Data {
    static constexpr MsgType type = MsgType::Data;
    std::uint32_t file_id = 0;
    std::uint64_t offset = 0;
    std::span<const std::byte> bytes{};
};

// C->S: every file has arrived; the server can drop the session.
struct Ack {
    static constexpr MsgType type = MsgType::Ack;
    bool operator==(const Ack&) const = default;
};

using Message = std::variant<Get, Ok, Err, Req, Data, Ack>;

[[nodiscard]] MsgType type_of(const Message& msg) noexcept;

// The frame payload for `msg` (the frame header is added by the frame layer).
[[nodiscard]] std::vector<std::byte> encode(const Message& msg);

// Parses and validates a payload of message type `tag`. Throws DecodeError
// for an unknown type or a malformed payload.
[[nodiscard]] Message decode(std::uint32_t tag, std::span<const std::byte> payload);

// Topic names: no '/', spaces or control characters, not "." or "..".
[[nodiscard]] bool is_valid_topic(std::string_view s) noexcept;

// File names: safe to create inside the output folder (no '/', no NUL,
// not "." or "..").
[[nodiscard]] bool is_valid_filename(std::string_view s) noexcept;

[[nodiscard]] std::string_view to_string(ErrCode code) noexcept;

}  // namespace axon::proto
