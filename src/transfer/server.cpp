#include "axon/transfer/server.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include "axon/fault/fault.hpp"
#include "axon/log.hpp"
#include "axon/protocol/bytes.hpp"
#include "axon/protocol/frame.hpp"
#include "sys_error.hpp"
#include "transfer/session_worker.hpp"

namespace axon {
namespace {

using namespace proto;

static_assert(std::atomic<bool>::is_always_lock_free, "stop() must be async-signal-safe");

// GET and LIST are a few hundred bytes; anything far bigger is not a client.
constexpr std::size_t max_request_len = 4096;

// A request the server refuses: sent to the client as Err, then the
// connection closes.
class Rejection : public std::runtime_error {
public:
    Rejection(ErrCode c, const std::string& reason) : std::runtime_error{reason}, code{c} {}
    ErrCode code;
};

[[noreturn]] void reject(ErrCode code, const std::string& reason) { throw Rejection{code, reason}; }

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

// LIST is answered here. GET opens or resumes a session, is answered with OK,
// and the connection is then handed to the session's worker for REQ, DATA
// and ACK.
void Server::handshake(Conn& conn, const std::string& who) {
    Socket& sock = conn.sock;
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
    hand_off(session, detail::Stream{
                          .id = conn.id,
                          .sock = take_socket(conn),
                          .fault = FaultPlan{config_.fault, conn.id,
                                             session->bytes_left() / get->connections}});
}

// Gives the connection to its session's worker, starting one if there is
// none (or the last one is just exiting).
void Server::hand_off(const std::shared_ptr<Session>& session, detail::Stream stream) {
    const std::lock_guard lock{workers_mu_};
    if (stopping_.load()) {
        return;  // shutting down: the stream's socket just closes
    }
    std::unique_ptr<detail::SessionWorker>& worker = workers_[session->id()];
    if (worker && worker->add(stream)) {
        return;
    }
    worker = std::make_unique<detail::SessionWorker>(
        session,
        detail::WorkerConfig{.idle_timeout = config_.idle_timeout,
                             .on_finish = [this](const Session& s, const std::string& who) {
                                 finish(s, who);
                             }},
        std::move(stream));
}

// Moves the socket out of `conn` under mu_, so close_all() never shuts a
// socket while it changes hands.
Socket Server::take_socket(Conn& conn) {
    const std::lock_guard lock{mu_};
    return std::move(conn.sock);
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
    std::size_t sessions = 0;
    {
        const std::lock_guard lock{workers_mu_};
        sessions = static_cast<std::size_t>(std::ranges::count_if(
            workers_, [](const auto& entry) { return !entry.second->exited(); }));
    }
    log_msg("stopping: closing {} active session(s)", sessions);
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
        handshake(conn, who);
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
    const std::lock_guard lock{mu_};
    conn.sock.shutdown();  // (unless handed off) the client sees EOF now; the fd closes on reap
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
    finished.clear();  // each thread has returned, so joining is instant

    std::vector<std::unique_ptr<detail::SessionWorker>> exited;
    {
        const std::lock_guard lock{workers_mu_};
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->second->exited()) {
                exited.push_back(std::move(it->second));
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // `exited` is destroyed here, outside the lock: each thread has returned.
}

void Server::close_all() noexcept {
    // Handshake threads first, so none hands a connection over afterwards.
    std::list<Conn> all;
    {
        const std::lock_guard lock{mu_};
        all.splice(all.end(), conns_);
        for (Conn& conn : all) {
            conn.sock.shutdown();  // wakes a thread blocked in recv or send
        }
    }
    all.clear();  // each jthread joins, then its socket closes

    std::unordered_map<std::uint64_t, std::unique_ptr<detail::SessionWorker>> workers;
    {
        const std::lock_guard lock{workers_mu_};
        workers.swap(workers_);
    }
    workers.clear();  // each worker closes its connections and joins
}

}  // namespace axon
