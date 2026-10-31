#include "transfer/session_worker.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <deque>
#include <format>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "axon/log.hpp"
#include "axon/protocol/bytes.hpp"
#include "axon/protocol/frame.hpp"
#include "axon/storage/file.hpp"
#include "sys_error.hpp"

namespace axon::detail {
namespace {

using namespace proto;
using Clock = std::chrono::steady_clock;

constexpr std::size_t max_request_len = 4096;  // REQ and ACK are small
constexpr int frames_per_turn = 16;            // keeps one busy connection from starving the rest
constexpr auto drain_time = std::chrono::seconds{2};  // how long a closing connection may linger
constexpr std::size_t data_start = frame_header_len + data_header_len;  // file bytes in a DATA frame

std::string errno_text(int err) { return std::generic_category().message(err); }

// One connection, as the worker sees it.
struct Link {
    explicit Link(Stream s) : stream{std::move(s)}, who{std::format("conn {}", stream.id)} {}

    Stream stream;
    std::string who;
    Clock::time_point last_active = Clock::now();

    std::vector<std::byte> in;  // received bytes not yet parsed into requests
    std::deque<Range> pending;  // requested ranges not yet started

    std::optional<Range> current;  // the range being sent
    File file;
    std::uint64_t pos = 0;

    std::vector<std::byte> out;  // the frame being written
    std::size_t out_pos = 0;
    Range out_data{};  // the file bytes in `out`, recorded once all of it is sent

    std::optional<std::vector<std::byte>> err;  // an ERR frame to send next, then close
    bool close_after_flush = false;
    // Closing gracefully: our side is shut, input is read and dropped until
    // the client closes too. Closing at once with unread input would make
    // the kernel send a reset and drop the last frame still queued.
    bool draining = false;
    Clock::time_point drain_until{};
    bool dead = false;
    std::string why;  // logged when the link is dropped
};

// The connections of one session and what to do with each of them.
class Loop {
public:
    Loop(Session& session, const WorkerConfig& config) : session_{session}, config_{config} {}

    void add(Stream stream) {
        Link& l = links_.emplace_back(std::move(stream));
        try {
            l.stream.sock.set_nonblocking();
        } catch (const std::exception& e) {
            kill(l, std::format("dropped: {}", e.what()));
        }
    }

    [[nodiscard]] bool empty() const { return links_.empty(); }

    // One turn: sends what can go without waiting, then waits for the next
    // event (input, room to send, a new connection, or an idle deadline).
    void turn(int wake_fd) {
        for (Link& l : links_) {
            pump(l);
        }
        drop_dead();
        if (links_.empty()) {
            return;
        }
        std::vector<pollfd> fds{{.fd = wake_fd, .events = POLLIN, .revents = 0}};
        auto deadline = Clock::time_point::max();
        for (const Link& l : links_) {
            fds.push_back({.fd = l.stream.sock.fd(),
                           .events = static_cast<short>(POLLIN | (has_output(l) ? POLLOUT : 0)),
                           .revents = 0});
            deadline = std::min(deadline, l.draining ? l.drain_until : l.last_active + config_.idle_timeout);
        }
        const auto wait = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        const int timeout = static_cast<int>(std::clamp<std::int64_t>(wait.count(), 0, 60'000));
        if (::poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            throw_errno("poll");
        }
        if (fds[0].revents != 0) {
            std::uint64_t count = 0;
            [[maybe_unused]] const ssize_t n = ::read(wake_fd, &count, sizeof count);
        }
        for (std::size_t i = 0; i < links_.size(); ++i) {
            Link& l = links_[i];
            const short ev = fds[i + 1].revents;
            if (!l.dead && (ev & (POLLIN | POLLHUP | POLLERR)) != 0) {
                read_input(l);
            }
            if (!l.dead && (ev & POLLOUT) != 0) {
                pump(l);
            }
        }
        const auto now = Clock::now();
        for (Link& l : links_) {
            if (l.dead) {
                continue;
            }
            if (l.draining) {
                if (now >= l.drain_until) {
                    kill(l, "");
                }
            } else if (now - l.last_active > config_.idle_timeout) {
                kill(l, std::format("dropped: idle for {} ms", config_.idle_timeout.count()));
            }
        }
        drop_dead();
    }

private:
    // Anything left to send: a frame in flight, a queued ERR, or ranges. Then
    // poll() must wait for POLLOUT too; otherwise a connection that used up
    // its frames_per_turn would never be woken to send the rest.
    static bool has_output(const Link& l) {
        return !l.draining && (l.out_pos < l.out.size() || l.err || l.close_after_flush ||
                               l.current || !l.pending.empty());
    }

    static void kill(Link& l, std::string why) {
        l.dead = true;
        l.why = std::move(why);
    }

    void drop_dead() {
        std::erase_if(links_, [](const Link& l) {
            if (l.dead && !l.why.empty()) {
                log_msg("{}: {}", l.who, l.why);
            }
            return l.dead;
        });
    }

    // Queues ERR as the next frame; the connection closes once it is sent.
    static void refuse(Link& l, ErrCode code, const std::string& reason) {
        log_msg("{}: refused, {}: {}", l.who, to_string(code), reason);
        l.err = encode_frame(Err{.code = code, .reason = reason});
        l.pending.clear();
        l.current.reset();
        l.file = File{};
        l.in.clear();
    }

    // Reads what has arrived and handles each complete request in it.
    void read_input(Link& l) {
        std::array<std::byte, 4096> buf{};
        for (;;) {
            const ssize_t n = ::recv(l.stream.sock.fd(), buf.data(), buf.size(), 0);
            if (n > 0 && l.draining) {
                continue;  // closing: drop whatever the client still sends
            }
            if (n > 0) {
                l.last_active = Clock::now();
                if (!l.err && !l.close_after_flush) {
                    l.in.insert(l.in.end(), buf.begin(), buf.begin() + n);
                    parse(l);
                }
                if (l.dead) {
                    return;  // ACK closed it
                }
                continue;
            }
            if (n == 0) {
                const bool quiet = finished_ || l.draining;
                kill(l, quiet ? "" : std::format("client left; session {:016x} kept for resume",
                                                 session_.id()));
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                kill(l, std::format("dropped: recv: {}", errno_text(errno)));
            }
            return;
        }
    }

    void parse(Link& l) {
        while (!l.err && l.in.size() >= frame_header_len) {
            const auto type = load_be<std::uint32_t>(std::span{l.in}.first<4>());
            const auto len = load_be<std::uint32_t>(std::span{l.in}.subspan<4, 4>());
            if (len > max_request_len) {
                refuse(l, ErrCode::BadRequest, std::format("frame of {} bytes", len));
                return;
            }
            if (l.in.size() < frame_header_len + len) {
                return;
            }
            Message msg;
            try {
                msg = decode(type, std::span{l.in}.subspan(frame_header_len, len));
            } catch (const DecodeError& e) {
                refuse(l, ErrCode::BadRequest, e.what());
                return;
            }
            l.in.erase(l.in.begin(), l.in.begin() + static_cast<std::ptrdiff_t>(frame_header_len + len));
            if (const auto* req = std::get_if<Req>(&msg)) {
                l.pending.insert(l.pending.end(), req->ranges.begin(), req->ranges.end());
            } else if (std::holds_alternative<Ack>(msg)) {
                finish(l);
                return;
            } else {
                refuse(l, ErrCode::BadRequest, "expected REQ or ACK");
                return;
            }
        }
    }

    // ACK: the client has every file. Closes all the session's connections.
    void finish(const Link& l) {
        if (!finished_) {
            finished_ = true;
            config_.on_finish(session_, l.who);
        }
        for (Link& other : links_) {
            kill(other, "");
        }
    }

    // After the last frame (an ERR, or one cut short): shut our side, so the
    // client reads everything sent and then end-of-stream.
    static void start_draining(Link& l) {
        if (!l.why.empty()) {
            log_msg("{}: {}", l.who, l.why);
            l.why.clear();
        }
        ::shutdown(l.stream.sock.fd(), SHUT_WR);
        l.close_after_flush = false;
        l.draining = true;
        l.drain_until = Clock::now() + drain_time;
    }

    // Sends as much as the socket takes now, up to frames_per_turn frames.
    void pump(Link& l) {
        for (int frames = 0; frames < frames_per_turn && !l.dead && !l.draining; ++frames) {
            if (l.out_pos == l.out.size()) {
                if (l.close_after_flush) {
                    start_draining(l);
                    return;
                }
                if (!next_frame(l)) {
                    return;
                }
            }
            const ssize_t n = ::send(l.stream.sock.fd(), l.out.data() + l.out_pos,
                                     l.out.size() - l.out_pos, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    kill(l, std::format("dropped: send: {}", errno_text(errno)));
                }
                return;
            }
            l.out_pos += static_cast<std::size_t>(n);
            l.last_active = Clock::now();
            if (l.out_pos < l.out.size()) {
                return;  // the socket buffer is full: wait for POLLOUT
            }
            if (l.out_data.length > 0) {  // the whole frame is with the kernel now
                session_.record_sent(l.out_data.file_id, l.out_data.offset, l.out_data.length);
                l.out_data = {};
            }
        }
    }

    // Puts the next frame into l.out: a queued ERR, or the next DATA of the
    // range being sent. False when there is nothing to send.
    bool next_frame(Link& l) {
        l.out.clear();
        l.out_pos = 0;
        for (;;) {
            if (l.err) {
                l.out = std::move(*l.err);
                l.err.reset();
                l.close_after_flush = true;
                return true;
            }
            if (!l.current) {
                if (l.pending.empty()) {
                    return false;
                }
                const Range r = l.pending.front();
                l.pending.pop_front();
                start_range(l, r);
                continue;
            }
            if (l.pos == l.current->offset + l.current->length) {
                l.current.reset();
                l.file = File{};
                continue;
            }
            if (build_data(l)) {
                return true;
            }
        }
    }

    void start_range(Link& l, const Range& r) {
        const Topic& topic = session_.topic();
        if (r.file_id >= topic.files.size()) {
            refuse(l, ErrCode::BadRange, std::format("no file with id {}", r.file_id));
            return;
        }
        const FileEntry& entry = topic.files[r.file_id];
        if (r.offset + r.length > entry.size) {
            refuse(l, ErrCode::BadRange,
                   std::format("{} bytes at offset {} run past the end of {} ({} bytes)", r.length,
                               r.offset, entry.name, entry.size));
            return;
        }
        // File errors are logged in full here; the client only learns which file.
        try {
            l.file = File::open_read(topic.file_path(r.file_id));
        } catch (const std::system_error& e) {
            log_msg("cannot serve file: {}", e.what());
            refuse(l, ErrCode::Internal, std::format("cannot read {}", entry.name));
            return;
        }
        l.current = r;
        l.pos = r.offset;
    }

    // Reads the next chunk of the current range straight into a DATA frame.
    // False if it had to refuse instead (the ERR is queued).
    bool build_data(Link& l) {
        const Range& r = *l.current;
        const std::string& name = session_.topic().files[r.file_id].name;
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(max_data_len, r.offset + r.length - l.pos));
        l.out.resize(data_start + want);
        std::size_t n = 0;
        try {
            n = l.file.read_at(std::span{l.out}.subspan(data_start, want), l.pos);
        } catch (const std::system_error& e) {
            log_msg("cannot serve file: {}", e.what());
            refuse(l, ErrCode::Internal, std::format("cannot read {}", name));
            return false;
        }
        if (n == 0) {
            refuse(l, ErrCode::Internal, std::format("{} shrank since the server started", name));
            return false;
        }
        l.out.resize(data_start + n);
        store_be(std::span{l.out}.first<4>(), static_cast<std::uint32_t>(MsgType::Data));
        store_be(std::span{l.out}.subspan<4, 4>(), static_cast<std::uint32_t>(data_header_len + n));
        store_be(std::span{l.out}.subspan<8, 4>(), r.file_id);
        store_be(std::span{l.out}.subspan<12, 8>(), l.pos);
        if (const std::size_t allowed = l.stream.fault.allow(n); allowed < n) {
            // The header still announces all n bytes: the client sees a cut frame.
            l.out.resize(data_start + allowed);
            l.out_data = {.file_id = r.file_id, .offset = l.pos, .length = allowed};
            l.close_after_flush = true;
            l.why = std::format("fault injection cut the connection after {} file bytes",
                                l.stream.fault.cut_at());
        } else {
            l.out_data = {.file_id = r.file_id, .offset = l.pos, .length = n};
            l.stream.fault.sent(n);
        }
        l.pos += n;
        return true;
    }

    Session& session_;
    const WorkerConfig& config_;
    std::vector<Link> links_;
    bool finished_ = false;
};

}  // namespace

SessionWorker::SessionWorker(std::shared_ptr<Session> session, WorkerConfig config, Stream first)
    : session_{std::move(session)},
      config_{std::move(config)},
      wake_{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)} {
    if (!wake_) {
        throw_errno("eventfd");
    }
    incoming_.push_back(std::move(first));
    thread_ = std::jthread{[this] { run(); }};
}

SessionWorker::~SessionWorker() { stop(); }  // thread_ joins as it is destroyed

bool SessionWorker::add(Stream& stream) {
    {
        const std::lock_guard lock{mu_};
        if (exiting_) {
            return false;
        }
        incoming_.push_back(std::move(stream));
    }
    wake();
    return true;
}

void SessionWorker::stop() noexcept {
    stop_.store(true);
    wake();
}

bool SessionWorker::exited() const {
    const std::lock_guard lock{mu_};
    return exiting_;
}

void SessionWorker::wake() noexcept {
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(wake_.get(), &one, sizeof one);
}

void SessionWorker::run() {
    Loop loop{*session_, config_};
    for (;;) {
        {
            const std::lock_guard lock{mu_};
            for (Stream& s : incoming_) {
                loop.add(std::move(s));
            }
            incoming_.clear();
            // Exit under the lock, so a connection handed over meanwhile is
            // either taken in above or refused by add().
            if (stop_.load() || loop.empty()) {
                exiting_ = true;
                return;  // the loop's sockets close as it goes
            }
        }
        try {
            loop.turn(wake_.get());
        } catch (const std::exception& e) {
            log_msg("session {:016x}: worker stopped: {}", session_->id(), e.what());
            const std::lock_guard lock{mu_};
            exiting_ = true;
            return;
        }
    }
}

}  // namespace axon::detail
