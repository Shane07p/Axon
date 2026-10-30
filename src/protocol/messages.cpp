#include "axon/protocol/messages.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "axon/protocol/bytes.hpp"

namespace axon::proto {
namespace {

constexpr std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();

// Smallest encoded FileEntry: name length (2) + 1-byte name + size (8) + crc (4).
constexpr std::size_t min_entry_len = 2 + 1 + 8 + 4;

// Smallest encoded TopicInfo: name length (2) + 1-byte name + files (4) + bytes (8).
constexpr std::size_t min_topic_len = 2 + 1 + 4 + 8;

[[noreturn]] void malformed(std::string_view what) {
    throw DecodeError("malformed " + std::string{what});
}

std::string printable_tag(std::uint32_t t) {
    std::string s(4, '?');
    for (std::size_t i = 0; i < 4; ++i) {
        const auto c = static_cast<char>(t >> (24 - 8 * i));
        if (c >= 0x20 && c < 0x7F) {
            s[i] = c;
        }
    }
    return s;
}

bool get_bool(ByteReader& r, std::string_view field) {
    switch (r.get_u8()) {
        case 0:
            return false;
        case 1:
            return true;
        default:
            malformed(std::string{field} + ": flag is not 0 or 1");
    }
}

// --- encoding ---

void put(ByteWriter& w, const Get& m) {
    w.put_u16(m.version);
    w.put_str(m.topic);
    w.put_u64(m.session_id);
    w.put_u8(static_cast<std::uint8_t>(m.want_list));
}

void put(ByteWriter& w, const Ok& m) {
    w.put_u64(m.session_id);
    w.put_u64(m.dataset_id);
    w.put_u8(static_cast<std::uint8_t>(m.files.has_value()));
    if (m.files) {
        w.put_u32(static_cast<std::uint32_t>(m.files->size()));
        for (const auto& f : *m.files) {
            w.put_str(f.name);
            w.put_u64(f.size);
            w.put_u32(f.crc);
        }
    }
}

void put(ByteWriter& w, const Err& m) {
    w.put_u16(static_cast<std::uint16_t>(m.code));
    w.put_str(m.reason);
}

void put(ByteWriter& w, const Req& m) {
    w.put_u16(static_cast<std::uint16_t>(m.ranges.size()));
    for (const auto& r : m.ranges) {
        w.put_u32(r.file_id);
        w.put_u64(r.offset);
        w.put_u64(r.length);
    }
}

void put(ByteWriter& w, const Data& m) {
    w.put_u32(m.file_id);
    w.put_u64(m.offset);
    w.put_bytes(m.bytes);
}

void put(ByteWriter& /*w*/, const Ack& /*m*/) {}

void put(ByteWriter& w, const List& m) { w.put_u16(m.version); }

void put(ByteWriter& w, const Topics& m) {
    w.put_u32(static_cast<std::uint32_t>(m.topics.size()));
    for (const auto& t : m.topics) {
        w.put_str(t.name);
        w.put_u32(t.files);
        w.put_u64(t.bytes);
    }
}

// --- decoding ---

Get decode_get(ByteReader& r) {
    Get m;
    m.version = r.get_u16();
    if (m.version != protocol_version) {
        return m;  // the rest may use another layout
    }
    m.topic = r.get_str();
    if (!is_valid_topic(m.topic)) {
        malformed("GET: invalid topic name");
    }
    m.session_id = r.get_u64();
    m.want_list = get_bool(r, "GET want_list");
    return m;
}

Ok decode_ok(ByteReader& r) {
    Ok m;
    m.session_id = r.get_u64();
    m.dataset_id = r.get_u64();
    if (!get_bool(r, "OK has_list")) {
        return m;
    }
    const std::uint32_t count = r.get_u32();
    // Checked before allocating, so a bogus count cannot reserve gigabytes.
    if (count > r.remaining() / min_entry_len) {
        malformed("OK: file count exceeds payload");
    }
    auto& files = m.files.emplace();
    files.reserve(count);
    std::unordered_set<std::string_view> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto name = r.get_str();
        if (!is_valid_filename(name)) {
            malformed("OK: unsafe file name");
        }
        if (!seen.insert(name).second) {
            malformed("OK: duplicate file name");
        }
        FileEntry& f = files.emplace_back();
        f.name = name;
        f.size = r.get_u64();
        f.crc = r.get_u32();
    }
    return m;
}

Err decode_err(ByteReader& r) {
    Err m;
    m.code = static_cast<ErrCode>(r.get_u16());  // unknown codes are kept, not rejected
    m.reason = r.get_str();
    return m;
}

Req decode_req(ByteReader& r) {
    const std::uint16_t count = r.get_u16();
    if (count == 0 || count > max_req_ranges) {
        malformed("REQ: range count must be 1 to 16");
    }
    Req m;
    m.ranges.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        Range& x = m.ranges.emplace_back();
        x.file_id = r.get_u32();
        x.offset = r.get_u64();
        x.length = r.get_u64();
        if (x.length == 0) {
            malformed("REQ: empty range");
        }
        if (x.offset > u64_max - x.length) {
            malformed("REQ: range overflows");
        }
    }
    return m;
}

Data decode_data(ByteReader& r) {
    Data m;
    m.file_id = r.get_u32();
    m.offset = r.get_u64();
    m.bytes = r.get_bytes(r.remaining());
    if (m.bytes.empty() || m.bytes.size() > max_data_len) {
        malformed("DATA: payload must be 1 to 65536 bytes");
    }
    if (m.offset > u64_max - m.bytes.size()) {
        malformed("DATA: range overflows");
    }
    return m;
}

List decode_list(ByteReader& r) { return List{.version = r.get_u16()}; }

Topics decode_topics(ByteReader& r) {
    const std::uint32_t count = r.get_u32();
    if (count > r.remaining() / min_topic_len) {
        malformed("TOPS: topic count exceeds payload");
    }
    Topics m;
    m.topics.reserve(count);
    std::unordered_set<std::string_view> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto name = r.get_str();
        if (!is_valid_topic(name)) {
            malformed("TOPS: invalid topic name");
        }
        if (!seen.insert(name).second) {
            malformed("TOPS: duplicate topic name");
        }
        TopicInfo& t = m.topics.emplace_back();
        t.name = name;
        t.files = r.get_u32();
        t.bytes = r.get_u64();
    }
    return m;
}

// A Get or List from another protocol version is decoded no further than its
// version: the rest may use another layout.
bool from_other_version(const Message& m) {
    if (const auto* get = std::get_if<Get>(&m)) {
        return get->version != protocol_version;
    }
    if (const auto* list = std::get_if<List>(&m)) {
        return list->version != protocol_version;
    }
    return false;
}

Message decode_body(std::uint32_t t, ByteReader& r) {
    switch (static_cast<MsgType>(t)) {
        case MsgType::Get:
            return decode_get(r);
        case MsgType::Ok:
            return decode_ok(r);
        case MsgType::Err:
            return decode_err(r);
        case MsgType::Req:
            return decode_req(r);
        case MsgType::Data:
            return decode_data(r);
        case MsgType::Ack:
            return Ack{};
        case MsgType::List:
            return decode_list(r);
        case MsgType::Topics:
            return decode_topics(r);
    }
    throw DecodeError("unknown message type '" + printable_tag(t) + "'");
}

bool is_dot_name(std::string_view s) { return s == "." || s == ".."; }

}  // namespace

MsgType type_of(const Message& msg) noexcept {
    return std::visit([](const auto& m) { return std::decay_t<decltype(m)>::type; }, msg);
}

std::vector<std::byte> encode(const Message& msg) {
    ByteWriter w;
    std::visit([&w](const auto& m) { put(w, m); }, msg);
    return std::move(w).take();
}

Message decode(std::uint32_t tag, std::span<const std::byte> payload) {
    ByteReader r{payload};
    Message msg = decode_body(tag, r);
    if (!from_other_version(msg)) {
        r.expect_end();
    }
    return msg;
}

bool is_valid_topic(std::string_view s) noexcept {
    if (s.empty() || s.size() > max_topic_len || is_dot_name(s)) {
        return false;
    }
    return std::ranges::none_of(s, [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return c == '/' || u <= 0x20 || u == 0x7F;  // slash, space, control
    });
}

bool is_valid_filename(std::string_view s) noexcept {
    if (s.empty() || s.size() > max_name_len || is_dot_name(s)) {
        return false;
    }
    return s.find_first_of(std::string_view{"/\0", 2}) == std::string_view::npos;
}

std::string_view to_string(ErrCode code) noexcept {
    switch (code) {
        case ErrCode::BadRequest:
            return "BAD_REQUEST";
        case ErrCode::BadVersion:
            return "BAD_VERSION";
        case ErrCode::UnknownTopic:
            return "UNKNOWN_TOPIC";
        case ErrCode::SessionNotFound:
            return "SESSION_NOT_FOUND";
        case ErrCode::TopicMismatch:
            return "TOPIC_MISMATCH";
        case ErrCode::BadRange:
            return "BAD_RANGE";
        case ErrCode::Internal:
            return "INTERNAL";
    }
    return "UNKNOWN_ERROR";
}

}  // namespace axon::proto
