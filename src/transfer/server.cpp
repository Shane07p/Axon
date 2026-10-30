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

#include "axon/fault/fault.hpp"
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

// Fault injection cut the connection on purpose.
class InjectedCut : public std::runtime_error {
public:
    explicit InjectedCut(std::uint64_t at)
        : std::runtime_error{std::format("fault injection cut the connection after {} file bytes", at)} {}
};

// Streams one requested range as DATA frames, recording them in `session`.
// When `fault` says so, the last frame is cut short and InjectedCut thrown.
void send_range(Socket& sock, Session& session, const Range& r, std::vector<std::byte>& buf,
                FaultPlan& fault) {
    const Topic& topic = session.topic();
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
        const Data data{.file_id = r.file_id, .offset = pos, .bytes = std::span{buf}.first(n)};
        if (const std::size_t allowed = fault.allow(n); allowed < n) {
            // The header announces all n bytes; only `allowed` of them follow.
            send_truncated(sock, data, data_header_len + allowed);
            session.record_sent(r.file_id, pos, allowed);
            throw InjectedCut{fault.cut_at()};
        }
        send_message(sock, data);
        session.record_sent(r.file_id, pos, n);
        fault.sent(n);
        pos += n;
    }
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

}  // namespace

Server::Server(Dataset dataset, Listener listener, ServerConfig config)
    : dataset_{std::move(dataset)},
      listener_{std::move(listener)},
      port_{listener_.local_port()},
      config_{config},
      sessions_{config.session_ttl},
      wake_{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)} {
    if (!wake_) {
        detail::throw_errno("eventfd");
    }
}

Server::~Server() { close_all(); }

ServerStats Server::stats() const {
    return {.sessions_finished = sessions_finished_.load(),
            .payload_bytes = payload_bytes_.load(),
            .redundant_bytes = redundant_bytes_.load()};
}

// One client: LIST -> TOPS, or GET -> OK, then REQ ... (answered with DATA)
// until ACK. A GET with a session id resumes that session.
void Server::serve_client(Socket& sock, std::uint64_t conn_id, const std::string& who) {
    MessageReader reader{max_request_len};
    const auto first = reader.read(sock);
    if (!first) {
        return;  // connected and left without a word
    }
    if (const auto* list = std::get_if<List>(&*first)) {
        check_version(list->version);
        send_message(sock, summarize(dataset_));
        log_msg("{}: LIST, {} topic(s)", who, dataset_.topics().size());
        return;
    }
    const auto* get = std::get_if<Get>(&*first);
    if (get == nullptr) {
        reject(ErrCode::BadRequest, "the first message must be GET or LIST");
    }
    check_version(get->version);

    std::shared_ptr<Session> session;
    Ok ok;
    if (get->session_id == 0) {
        const Topic* topic = dataset_.find(get->topic);
        if (topic == nullptr) {
            reject(ErrCode::UnknownTopic, std::format("no topic named '{}'", get->topic));
        }
        session = sessions_.create(*topic);
        ok.files = topic->files;
        log_msg("{}: GET {}, new session {:016x}, {} file(s)", who, topic->name, session->id(),
                topic->files.size());
    } else {
        session = sessions_.find(get->session_id);
        if (session == nullptr) {
            reject(ErrCode::SessionNotFound, "unknown, finished or expired session");
        }
        if (session->topic().name != get->topic) {
            reject(ErrCode::TopicMismatch,
                   std::format("that session is for topic '{}'", session->topic().name));
        }
        if (get->want_list) {
            ok.files = session->topic().files;
        }
        log_msg("{}: resuming session {:016x}", who, session->id());
    }
    session->connected();
    ok.session_id = session->id();
    send_message(sock, ok);
    // Like the C version: each of the client's connections carries about 1/n of
    // what is left, so the cut point is drawn over that share.
    FaultPlan fault{config_.fault, conn_id, session->bytes_left() / get->connections};

    std::vector<std::byte> buf(max_data_len);
    while (const auto msg = reader.read(sock)) {
        if (const auto* req = std::get_if<Req>(&*msg)) {
            for (const Range& r : req->ranges) {
                send_range(sock, *session, r, buf, fault);
            }
        } else if (std::holds_alternative<Ack>(*msg)) {
            finish(*session, who);
            return;
        } else {
            reject(ErrCode::BadRequest, "expected REQ or ACK");
        }
    }
    log_msg("{}: client left before ACK; session {:016x} kept for resume", who, session->id());
}

void Server::finish(const Session& session, const std::string& who) {
    sessions_.finish(session.id());
    const SessionStats st = session.stats();
    sessions_finished_.fetch_add(1);
    payload_bytes_.fetch_add(st.payload);
    redundant_bytes_.fetch_add(st.redundant);
    const std::chrono::duration<double> took = Session::Clock::now() - session.started();
    log_msg("{}: session {:016x} done: {} bytes in {:.2f} s ({:.1f} MB/s), {} resent, "
            "{} connection(s)",
            who, session.id(), st.payload, took.count(),
            static_cast<double>(st.payload) / 1e6 / std::max(took.count(), 1e-9), st.redundant,
            st.connections);
}

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
    listener_ = Listener{};  // clients still in the accept queue get a reset, not a hang
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
        serve_client(conn.sock, conn.id, who);
    } catch (const InjectedCut& e) {
        log_msg("{}: {}", who, e.what());
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
