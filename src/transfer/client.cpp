#include "axon/transfer/client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <format>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "axon/crc32.hpp"
#include "axon/fault/fault.hpp"
#include "axon/net/socket.hpp"
#include "axon/protocol/bytes.hpp"
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

// Everything about a download that outlives one connection.
struct Download {
    struct Progress {
        std::uint64_t got = 0;  // bytes in the temporary file, all from complete frames
        std::uint32_t crc = 0;  // CRC-32 of those bytes
        bool done = false;      // checked and renamed into place
        File part{};            // stays open across reconnects
    };

    fs::path dir{};
    std::uint64_t session = 0;  // 0: none yet
    std::vector<FileEntry> files{};
    std::vector<Progress> progress{};
    std::uint64_t received = 0;  // file bytes received over all attempts
    std::size_t completed = 0;   // files completed over all attempts
    std::vector<std::byte> buf = std::vector<std::byte>(max_data_len);  // receive buffer

    // Something to tell an attempt that made progress from one that did not.
    [[nodiscard]] std::uint64_t mark() const { return received + completed; }

    // The server forgot the session: drop it and everything received under it.
    void reset() {
        session = 0;
        files.clear();
        progress.clear();
    }
};

// Receives the rest of file `id` (the next range the server sends, unless
// nothing is missing), then checks it and renames it into place. DATA
// payloads are written as they arrive, not frame by frame, so a connection
// cut mid-frame keeps every byte that got through.
void receive_rest(Socket& sock, MessageReader& reader, Download& d, std::uint32_t id) {
    Download::Progress& p = d.progress[id];
    const FileEntry& entry = d.files[id];
    const fs::path part = temp_path(d.dir, id);
    if (!p.part) {
        p.part = File::open_write(part);
        p.part.truncate(0);  // a stale temp file from an earlier run
    }
    while (p.got < entry.size) {
        const auto header = read_frame_header(sock);
        if (!header) {
            throw ConnectionClosed("the server closed the connection");
        }
        if (header->type != static_cast<std::uint32_t>(MsgType::Data)) {
            const Message msg = reader.read_payload(sock, *header);
            if (const auto* err = std::get_if<Err>(&msg)) {
                throw ServerError(err->code, err->reason);
            }
            protocol_error(std::format("expected DATA for {}", printable(entry.name)));
        }
        if (header->length <= data_header_len || header->length > data_header_len + max_data_len) {
            protocol_error(std::format("DATA frame of {} bytes", header->length));
        }
        std::array<std::byte, data_header_len> head{};
        if (!sock.recv_all(head)) {
            throw ConnectionClosed("connection closed inside a DATA frame");
        }
        ByteReader fields{head};
        const std::uint32_t file_id = fields.get_u32();
        const std::uint64_t offset = fields.get_u64();
        std::size_t left = header->length - data_header_len;
        if (file_id != id || offset != p.got) {
            protocol_error(std::format("expected DATA for file {} at offset {}, got file {} at {}", id,
                                       p.got, file_id, offset));
        }
        if (left > entry.size - p.got) {
            protocol_error(std::format("DATA runs past the end of {}", printable(entry.name)));
        }
        while (left > 0) {
            const std::size_t n = sock.recv_some(std::span{d.buf}.first(std::min(left, d.buf.size())));
            if (n == 0) {
                throw ConnectionClosed(std::format("connection closed {} bytes into a DATA frame",
                                                   header->length - data_header_len - left));
            }
            const auto piece = std::span{d.buf}.first(n);
            p.part.write_at(piece, p.got);
            p.crc = crc32_update(p.crc, piece);
            p.got += n;
            d.received += n;
            left -= n;
        }
    }
    p.part = File{};  // close before renaming or deleting
    if (p.crc != entry.crc) {
        std::error_code ec;
        fs::remove(part, ec);
        throw IntegrityError(std::format("{} is corrupt: CRC-32 {:08x}, expected {:08x}",
                                         printable(entry.name), p.crc, entry.crc));
    }
    fs::rename(part, d.dir / entry.name);
    p.done = true;
    ++d.completed;
}

// One connection's worth of work: joins (or rejoins) the session and asks for
// every unfinished file from the byte it has. Returns once all are done and
// ACK is sent.
void run_connection(const FetchRequest& req, Download& d, bool& connected_once) {
    Socket sock = Socket::connect(req.host, req.port);
    connected_once = true;
    sock.set_timeouts(req.timeout);
    MessageReader reader;

    send_message(sock, Get{.topic = req.topic, .session_id = d.session, .want_list = d.session == 0});
    const Message reply = next(sock, reader);
    const auto* ok = std::get_if<Ok>(&reply);
    if (ok == nullptr) {
        protocol_error("expected OK");
    }
    if (d.session == 0) {
        if (!ok->files) {
            protocol_error("expected OK with the file list");
        }
        d.session = ok->session_id;
        d.files = *ok->files;
        d.progress = std::vector<Download::Progress>(d.files.size());
        fs::create_directories(d.dir);
    } else if (ok->session_id != d.session) {
        protocol_error("the server resumed a different session");
    }

    // Up to max_req_ranges unfinished files per REQ; empty files need no range.
    for (std::size_t first = 0; first < d.files.size();) {
        Req batch;
        std::vector<std::uint32_t> ids;
        std::size_t id = first;
        for (; id < d.files.size() && ids.size() < max_req_ranges; ++id) {
            const Download::Progress& p = d.progress[id];
            if (p.done) {
                continue;
            }
            ids.push_back(static_cast<std::uint32_t>(id));
            if (p.got < d.files[id].size) {
                batch.ranges.push_back({.file_id = static_cast<std::uint32_t>(id),
                                        .offset = p.got,
                                        .length = d.files[id].size - p.got});
            }
        }
        if (!batch.ranges.empty()) {
            send_message(sock, batch);
        }
        for (const std::uint32_t file : ids) {
            receive_rest(sock, reader, d, file);
        }
        first = id;
    }
    send_message(sock, Ack{});
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
    Download d{.dir = req.output / req.topic};
    SplitMix64 jitter{static_cast<std::uint64_t>(start.time_since_epoch().count())};
    bool connected_once = false;
    std::size_t connections = 0;
    int failures = 0;  // attempts in a row that received nothing
    std::chrono::milliseconds delay = req.retry.first_delay;

    for (;;) {
        const std::uint64_t mark = d.mark();
        std::exception_ptr error;
        std::string reason;
        try {
            ++connections;
            run_connection(req, d, connected_once);
            break;
        } catch (const ServerError& e) {
            if (e.code != ErrCode::SessionNotFound || d.session == 0) {
                throw;
            }
            error = std::current_exception();
            reason = e.what();
            d.reset();  // the session expired or the server restarted: start over
        } catch (const ConnectionClosed& e) {
            error = std::current_exception();
            reason = e.what();
        } catch (const NetworkError& e) {
            if (!connected_once) {
                throw;  // never reached the server: likely a wrong address
            }
            error = std::current_exception();
            reason = e.what();
        }
        if (d.mark() > mark) {
            failures = 0;
            delay = req.retry.first_delay;
        } else if (++failures >= req.retry.max_failures) {
            std::rethrow_exception(error);
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            delay * (0.5 + 0.5 * jitter.unit()));
        if (req.on_retry) {
            req.on_retry(reason, wait);
        }
        std::this_thread::sleep_for(wait);
        delay = std::min(delay * 2, req.retry.max_delay);
    }

    FetchStats stats{.files = d.files.size(), .connections = connections};
    for (const FileEntry& f : d.files) {
        stats.bytes += f.size;
    }
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
