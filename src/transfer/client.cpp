#include "axon/transfer/client.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "axon/crc32.hpp"
#include "axon/net/socket.hpp"
#include "axon/protocol/frame.hpp"
#include "axon/storage/file.hpp"

namespace axon {
namespace {

namespace fs = std::filesystem;
using namespace proto;

// Server-supplied text is shown in a terminal: drop control characters so it
// cannot carry escape sequences.
std::string printable(std::string_view s) {
    std::string out{s};
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            c = '?';
        }
    }
    return out;
}

[[noreturn]] void protocol_error(const std::string& what) {
    throw ProtocolError("protocol error: " + what);
}

// The next message; ERR becomes ServerError. A Data result views the
// reader's buffer, so use it before calling next() again.
Message next(Socket& sock, MessageReader& reader) {
    auto msg = reader.read(sock);
    if (!msg) {
        throw ConnectionClosed("the server closed the connection");
    }
    if (const auto* err = std::get_if<Err>(&*msg)) {
        throw ServerError(err->code, err->reason);
    }
    return std::move(*msg);
}

fs::path temp_path(const fs::path& dir, std::size_t id) {
    return dir / std::format(".axon-{}.part", id);
}

// Receives file `id` (the next range the server sends, if it is not empty)
// into its temporary file, checks it, and renames it into place.
std::uint64_t receive_file(Socket& sock, MessageReader& reader, const fs::path& dir,
                           std::uint32_t id, const FileEntry& entry) {
    const fs::path part = temp_path(dir, id);
    File file = File::open_write(part);
    file.truncate(0);  // a stale temp file from an earlier run
    std::uint64_t got = 0;
    std::uint32_t crc = 0;
    while (got < entry.size) {
        const Message msg = next(sock, reader);
        const auto* data = std::get_if<Data>(&msg);
        if (data == nullptr) {
            protocol_error(std::format("expected DATA for {}", printable(entry.name)));
        }
        if (data->file_id != id || data->offset != got) {
            protocol_error(std::format("expected DATA for file {} at offset {}, got file {} at {}", id,
                                       got, data->file_id, data->offset));
        }
        if (data->bytes.size() > entry.size - got) {
            protocol_error(std::format("DATA runs past the end of {}", printable(entry.name)));
        }
        file.write_at(data->bytes, got);
        crc = crc32_update(crc, data->bytes);
        got += data->bytes.size();
    }
    file = File{};  // close before renaming or deleting
    if (crc != entry.crc) {
        std::error_code ec;
        fs::remove(part, ec);
        throw IntegrityError(std::format("{} is corrupt: CRC-32 {:08x}, expected {:08x}",
                                         printable(entry.name), crc, entry.crc));
    }
    fs::rename(part, dir / entry.name);
    return got;
}

}  // namespace

ServerError::ServerError(ErrCode c, const std::string& reason)
    : std::runtime_error{std::format("server refused: {}: {}", to_string(c), printable(reason))},
      code{c} {}

FetchStats fetch(const FetchRequest& req) {
    if (!is_valid_topic(req.topic)) {
        throw std::invalid_argument(std::format("invalid topic name '{}'", printable(req.topic)));
    }
    const auto start = std::chrono::steady_clock::now();
    Socket sock = Socket::connect(req.host, req.port);
    sock.set_timeouts(req.timeout);
    MessageReader reader;

    send_message(sock, Get{.topic = req.topic, .want_list = true});
    const Message reply = next(sock, reader);
    const auto* ok = std::get_if<Ok>(&reply);
    if (ok == nullptr || !ok->files) {
        protocol_error("expected OK with the file list");
    }
    const std::vector<FileEntry>& files = *ok->files;
    const fs::path dir = req.output / req.topic;
    fs::create_directories(dir);

    // Ask for up to max_req_ranges files at a time; empty files need no request.
    FetchStats stats;
    for (std::size_t first = 0; first < files.size();) {
        const std::size_t last = std::min(files.size(), first + max_req_ranges);
        Req batch;
        for (std::size_t id = first; id < last; ++id) {
            if (files[id].size > 0) {
                batch.ranges.push_back(
                    {.file_id = static_cast<std::uint32_t>(id), .offset = 0, .length = files[id].size});
            }
        }
        if (!batch.ranges.empty()) {
            send_message(sock, batch);
        }
        for (std::size_t id = first; id < last; ++id) {
            stats.bytes += receive_file(sock, reader, dir, static_cast<std::uint32_t>(id), files[id]);
            ++stats.files;
        }
        first = last;
    }
    send_message(sock, Ack{});
    stats.elapsed = std::chrono::steady_clock::now() - start;
    return stats;
}

std::vector<TopicInfo> list_topics(const std::string& host, std::uint16_t port,
                                   std::chrono::milliseconds timeout) {
    Socket sock = Socket::connect(host, port);
    sock.set_timeouts(timeout);
    MessageReader reader;
    send_message(sock, List{});
    Message reply = next(sock, reader);
    auto* topics = std::get_if<Topics>(&reply);
    if (topics == nullptr) {
        protocol_error("expected TOPS");
    }
    return std::move(topics->topics);
}

}  // namespace axon
