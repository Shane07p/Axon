#include "axon/transfer/server.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <format>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "axon/log.hpp"
#include "axon/protocol/bytes.hpp"
#include "axon/protocol/frame.hpp"
#include "axon/storage/file.hpp"
#include "sys_error.hpp"

namespace axon {
namespace {

using namespace proto;

static_assert(std::atomic<bool>::is_always_lock_free, "stop() must be async-signal-safe");

// GET and REQ are a few hundred bytes; anything far bigger is not a client.
constexpr std::size_t max_request_len = 4096;

// A request the server refuses: sent to the client as Err, then the
// connection closes.
class Rejection : public std::runtime_error {
public:
    Rejection(ErrCode c, const std::string& reason) : std::runtime_error{reason}, code{c} {}
    ErrCode code;
};

[[noreturn]] void reject(ErrCode code, const std::string& reason) { throw Rejection{code, reason}; }

// Streams one requested range as DATA frames. Returns the bytes sent.
std::uint64_t send_range(Socket& sock, const Topic& topic, const Range& r,
                         std::vector<std::byte>& buf) {
    if (r.file_id >= topic.files.size()) {
        reject(ErrCode::BadRange, std::format("no file with id {}", r.file_id));
    }
    const FileEntry& entry = topic.files[r.file_id];
    if (r.offset + r.length > entry.size) {
        reject(ErrCode::BadRange, std::format("{} bytes at offset {} run past the end of {} ({} bytes)",
                                              r.length, r.offset, entry.name, entry.size));
    }
    // File errors are logged in full here; the client only learns which file.
    File file;
    try {
        file = File::open_read(topic.file_path(r.file_id));
    } catch (const std::system_error& e) {
        log_msg("cannot serve file: {}", e.what());
        reject(ErrCode::Internal, std::format("cannot read {}", entry.name));
    }
    const std::uint64_t end = r.offset + r.length;
    for (std::uint64_t pos = r.offset; pos < end;) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), end - pos));
        std::size_t n = 0;
        try {
            n = file.read_at(std::span{buf}.first(want), pos);
        } catch (const std::system_error& e) {
            log_msg("cannot serve file: {}", e.what());
            reject(ErrCode::Internal, std::format("cannot read {}", entry.name));
        }
        if (n == 0) {
            reject(ErrCode::Internal, std::format("{} shrank since the server started", entry.name));
        }
        send_message(sock, Data{.file_id = r.file_id, .offset = pos, .bytes = std::span{buf}.first(n)});
        pos += n;
    }
    return r.length;
}

void check_version(std::uint16_t version) {
    if (version != protocol_version) {
        reject(ErrCode::BadVersion, std::format("this server speaks protocol version {}, not {}",
                                                protocol_version, version));
    }
}

Topics summarize(const Dataset& dataset) {
    Topics out;
    for (const Topic& t : dataset.topics()) {
        TopicInfo& info = out.topics.emplace_back();
        info.name = t.name;
        info.files = static_cast<std::uint32_t>(t.files.size());
        for (const FileEntry& f : t.files) {
            info.bytes += f.size;
        }
    }
    return out;
}

// One client: LIST -> TOPS, or GET -> OK, then REQ ... (answered with DATA)
// until ACK.
void serve_client(Socket& sock, const Dataset& dataset, const std::string& who) {
    MessageReader reader{max_request_len};
    const auto first = reader.read(sock);
    if (!first) {
        return;  // connected and left without a word
    }
    if (const auto* list = std::get_if<List>(&*first)) {
        check_version(list->version);
        send_message(sock, summarize(dataset));
        log_msg("{}: LIST, {} topic(s)", who, dataset.topics().size());
        return;
    }
    const auto* get = std::get_if<Get>(&*first);
    if (get == nullptr) {
        reject(ErrCode::BadRequest, "the first message must be GET or LIST");
    }
    check_version(get->version);
    if (get->session_id != 0) {
        reject(ErrCode::SessionNotFound, "this server does not resume sessions");
    }
    const Topic* topic = dataset.find(get->topic);
    if (topic == nullptr) {
        reject(ErrCode::UnknownTopic, std::format("no topic named '{}'", get->topic));
    }
    send_message(sock, Ok{.files = topic->files});
    log_msg("{}: GET {}, {} file(s)", who, topic->name, topic->files.size());

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::byte> buf(max_data_len);
    std::uint64_t sent = 0;
    while (const auto msg = reader.read(sock)) {
        if (const auto* req = std::get_if<Req>(&*msg)) {
            for (const Range& r : req->ranges) {
                sent += send_range(sock, *topic, r, buf);
            }
        } else if (std::holds_alternative<Ack>(*msg)) {
            const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
            log_msg("{}: done, {} bytes in {:.2f} s ({:.1f} MB/s)", who, sent, took.count(),
                    static_cast<double>(sent) / 1e6 / std::max(took.count(), 1e-9));
            return;
        } else {
            reject(ErrCode::BadRequest, "expected REQ or ACK");
        }
    }
    log_msg("{}: client left before ACK, after {} bytes", who, sent);
}

}  // namespace

Server::Server(Dataset dataset, Listener listener, ServerConfig config)
    : dataset_{std::move(dataset)},
      listener_{std::move(listener)},
      config_{config},
      wake_{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)} {
    if (!wake_) {
        detail::throw_errno("eventfd");
    }
}

Server::~Server() { close_all(); }

void Server::stop() noexcept {
    stopping_.store(true);
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(wake_.get(), &one, sizeof one);  // async-signal-safe
}

void Server::run() {
    while (!stopping_.load()) {
        std::array<pollfd, 2> fds{{
            {.fd = listener_.fd(), .events = POLLIN, .revents = 0},
            {.fd = wake_.get(), .events = POLLIN, .revents = 0},
        }};
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            detail::throw_errno("poll");
        }
        if (fds[1].revents != 0) {
            break;
        }
        try {
            if (auto sock = listener_.try_accept()) {
                reap();
                spawn(std::move(*sock));
            }
        } catch (const std::system_error& e) {
            // e.g. out of file descriptors: back off instead of spinning on poll().
            log_msg("cannot accept: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
    std::size_t open = 0;
    {
        const std::lock_guard lock{mu_};
        open = static_cast<std::size_t>(
            std::ranges::count_if(conns_, [](const Conn& c) { return !c.done.load(); }));
    }
    log_msg("stopping: closing {} connection(s)", open);
    close_all();
}

void Server::spawn(Socket sock) {
    const std::lock_guard lock{mu_};
    Conn& conn = conns_.emplace_back();
    conn.id = next_id_++;
    conn.sock = std::move(sock);
    try {
        conn.thread = std::jthread{[this, &conn] {
            serve(conn);
            conn.done.store(true);
        }};
    } catch (const std::system_error& e) {
        log_msg("conn {}: cannot start a thread: {}", conn.id, e.what());
        conns_.pop_back();
    }
}

void Server::serve(Conn& conn) {
    const std::string who = std::format("conn {}", conn.id);
    try {
        conn.sock.set_timeouts(config_.idle_timeout);
        serve_client(conn.sock, dataset_, who);
    } catch (const Rejection& r) {
        log_msg("{}: refused, {}: {}", who, proto::to_string(r.code), r.what());
        try {
            proto::send_message(conn.sock, proto::Err{.code = r.code, .reason = r.what()});
        } catch (const std::exception&) {  // the client is gone; nothing more to tell it
        }
    } catch (const DecodeError& e) {
        log_msg("{}: bad message: {}", who, e.what());
        try {
            proto::send_message(conn.sock,
                                proto::Err{.code = proto::ErrCode::BadRequest, .reason = e.what()});
        } catch (const std::exception&) {
        }
    } catch (const std::exception& e) {  // closed, reset or timed out
        if (!stopping_.load()) {
            log_msg("{}: dropped: {}", who, e.what());
        }
    }
    conn.sock.shutdown();  // the client sees EOF now; the fd closes when the Conn is reaped
}

void Server::reap() {
    std::list<Conn> finished;
    {
        const std::lock_guard lock{mu_};
        for (auto it = conns_.begin(); it != conns_.end();) {
            const auto next = std::next(it);
            if (it->done.load()) {
                finished.splice(finished.end(), conns_, it);
            }
            it = next;
        }
    }
    // `finished` is destroyed here: each thread has returned, so joining is instant.
}

void Server::close_all() noexcept {
    std::list<Conn> all;
    {
        const std::lock_guard lock{mu_};
        all.splice(all.end(), conns_);
    }
    for (Conn& conn : all) {
        conn.sock.shutdown();  // wakes a thread blocked in recv or send
    }
    // `all` is destroyed here: each jthread joins, then its socket closes.
}

}  // namespace axon
