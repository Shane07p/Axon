#include "axon/transfer/client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
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
using Clock = std::chrono::steady_clock;

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

// The next message; ERR becomes ServerError.
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

// When one connection retries: exponential backoff with jitter, and a limit
// on attempts in a row that make no progress.
class Backoff {
public:
    Backoff(const RetryPolicy& policy, std::uint64_t seed)
        : policy_{policy}, delay_{policy.first_delay}, jitter_{seed} {}

    // After a failed attempt: false once it is time to give up.
    bool failed(bool made_progress) {
        if (made_progress) {
            failures_ = 0;
            delay_ = policy_.first_delay;
            return true;
        }
        return ++failures_ < policy_.max_failures;
    }

    std::chrono::milliseconds next_wait() {
        const auto wait =
            std::chrono::duration_cast<std::chrono::milliseconds>(delay_ * (0.5 + 0.5 * jitter_.unit()));
        delay_ = std::min(delay_ * 2, policy_.max_delay);
        return wait;
    }

private:
    const RetryPolicy& policy_;
    std::chrono::milliseconds delay_;
    SplitMix64 jitter_;
    int failures_ = 0;
};

enum class FileState { Todo, Busy, Done };

struct FileProgress {
    FileState state = FileState::Todo;  // guarded by Download::mu_
    // Touched only by the connection holding the file Busy:
    std::uint64_t got = 0;  // bytes in the temporary file
    std::uint32_t crc = 0;  // CRC-32 of those bytes
    File part{};            // stays open while the file is unfinished
};

// One download: a session shared by several connections, each on its own
// thread, taking files from a common queue.
class Download {
public:
    explicit Download(const FetchRequest& req)
        : req_{req}, dir_{req.output / req.topic}, seed_{static_cast<std::uint64_t>(
                                                       Clock::now().time_since_epoch().count())} {}

    FetchStats run();

private:
    // A connection's socket and reader, kept together.
    struct Link {
        Socket sock;
        MessageReader reader;
    };

    Link open_session();  // a new session; retried once a session was lost
    Link connect(const Get& get);
    void work(Link first);
    void worker(std::size_t index, std::optional<Link> link);
    void serve_files(Link& link, std::vector<std::uint32_t>& mine, std::uint64_t& received);
    void receive_rest(Link& link, std::uint32_t id, std::uint64_t& received,
                      std::vector<std::byte>& buf);

    // Queue operations, all under mu_.
    std::vector<std::uint32_t> claim(bool& send_ack);
    void give_back(std::vector<std::uint32_t>& ids);
    void finish_file(std::uint32_t id);
    void abort(std::exception_ptr fatal, bool session_lost);
    bool is_aborted();
    bool all_done();
    bool sleep_unless_aborted(std::chrono::milliseconds wait);

    // Tells the server how many connections share the download.
    std::uint16_t connections_hint() const {
        return static_cast<std::uint16_t>(std::min<std::size_t>(req_.connections, 0xFFFF));
    }
    void track(Socket* sock, bool live);
    void reset();

    const FetchRequest& req_;
    const fs::path dir_;
    const std::uint64_t seed_;
    bool connected_once_ = false;  // only touched before the workers start
    std::atomic<std::size_t> connections_{0};

    // Set while no worker runs, read-only while they do.
    std::uint64_t session_ = 0;
    std::vector<FileEntry> files_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<FileProgress> progress_;  // states guarded by mu_; see FileProgress
    std::deque<std::uint32_t> todo_;      // guarded by mu_
    std::size_t done_ = 0;                // guarded by mu_
    std::vector<Socket*> live_;           // guarded by mu_: sockets to shut on abort
    bool aborted_ = false;                // guarded by mu_
    bool session_lost_ = false;           // guarded by mu_
    bool ack_claimed_ = false;            // guarded by mu_
    std::exception_ptr fatal_;            // guarded by mu_
    std::exception_ptr last_error_;       // guarded by mu_
};

FetchStats Download::run() {
    const auto start = Clock::now();
    for (;;) {
        work(open_session());
        // Every worker has finished; nothing else touches the state now.
        if (fatal_) {
            std::rethrow_exception(fatal_);
        }
        if (done_ == files_.size()) {
            break;
        }
        if (!session_lost_) {
            std::rethrow_exception(last_error_);  // every connection gave up
        }
        reset();  // the server forgot the session: start over
    }
    FetchStats stats{.files = files_.size(), .connections = connections_.load()};
    for (const FileEntry& f : files_) {
        stats.bytes += f.size;
    }
    stats.elapsed = Clock::now() - start;
    return stats;
}

Download::Link Download::connect(const Get& get) {
    Link link{.sock = Socket::connect(req_.host, req_.port), .reader = MessageReader{}};
    ++connections_;
    link.sock.set_timeouts(req_.timeout);
    send_message(link.sock, get);
    return link;
}

Download::Link Download::open_session() {
    Backoff backoff{req_.retry, seed_};
    for (;;) {
        try {
            Link link =
                connect(Get{.topic = req_.topic, .connections = connections_hint(), .want_list = true});
            connected_once_ = true;
            const Message reply = next(link.sock, link.reader);
            const auto* ok = std::get_if<Ok>(&reply);
            if (ok == nullptr || !ok->files) {
                protocol_error("expected OK with the file list");
            }
            session_ = ok->session_id;
            files_ = *ok->files;
            progress_ = std::vector<FileProgress>(files_.size());
            for (std::uint32_t id = 0; id < files_.size(); ++id) {
                todo_.push_back(id);
            }
            fs::create_directories(dir_);
            return link;
        } catch (const NetworkError&) {
            if (!connected_once_ || !backoff.failed(false)) {
                throw;  // a wrong address fails at once
            }
        } catch (const ConnectionClosed&) {
            if (!backoff.failed(false)) {
                throw;
            }
        }
        std::this_thread::sleep_for(backoff.next_wait());
    }
}

void Download::work(Link first) {
    // No more connections than there are files to share out.
    const std::size_t n = std::clamp<std::size_t>(req_.connections, 1, std::max<std::size_t>(todo_.size(), 1));
    std::vector<std::jthread> workers;
    workers.reserve(n);
    workers.emplace_back([this, link = std::move(first)]() mutable { worker(0, std::move(link)); });
    for (std::size_t i = 1; i < n; ++i) {
        workers.emplace_back([this, i] { worker(i, std::nullopt); });
    }
}  // the jthreads join here

void Download::worker(std::size_t index, std::optional<Link> link) {
    Backoff backoff{req_.retry, seed_ ^ (index * 0x9E3779B97F4A7C15ULL)};
    std::vector<std::uint32_t> mine;  // files this connection holds Busy
    for (;;) {
        std::uint64_t received = 0;
        std::exception_ptr error;
        std::string reason;
        try {
            if (!link) {
                if (all_done()) {
                    return;  // the others finished meanwhile: no need to reconnect
                }
                link = connect(
                    Get{.topic = req_.topic, .session_id = session_, .connections = connections_hint()});
                track(&link->sock, true);
                const Message reply = next(link->sock, link->reader);
                const auto* ok = std::get_if<Ok>(&reply);
                if (ok == nullptr || ok->session_id != session_) {
                    protocol_error("expected OK for our session");
                }
            } else {
                track(&link->sock, true);
            }
            serve_files(*link, mine, received);
            track(&link->sock, false);
            return;
        } catch (const ServerError& e) {
            if (e.code == ErrCode::SessionNotFound) {
                abort(nullptr, true);
            } else {
                abort(std::current_exception(), false);
            }
        } catch (const ConnectionClosed& e) {
            error = std::current_exception();
            reason = e.what();
        } catch (const NetworkError& e) {
            error = std::current_exception();
            reason = e.what();
        } catch (...) {
            abort(std::current_exception(), false);
        }
        if (link) {
            track(&link->sock, false);
            link.reset();
        }
        give_back(mine);
        if (!error || is_aborted()) {
            return;  // a fatal error or a lost session ends every connection
        }
        if (!backoff.failed(received > 0)) {
            const std::lock_guard lock{mu_};
            last_error_ = error;  // this connection gives up; the others may carry on
            return;
        }
        const auto wait = backoff.next_wait();
        if (req_.on_retry) {
            req_.on_retry(reason, wait);
        }
        if (!sleep_unless_aborted(wait)) {
            return;
        }
    }
}

// Claims batches of files and receives them until none are left.
void Download::serve_files(Link& link, std::vector<std::uint32_t>& mine, std::uint64_t& received) {
    std::vector<std::byte> buf(max_data_len);
    for (;;) {
        bool send_ack = false;
        mine = claim(send_ack);
        if (mine.empty()) {
            if (send_ack) {
                try {
                    // A courtesy that lets the server drop the session: the
                    // files are already complete, so a failure here is harmless.
                    send_message(link.sock, Ack{});
                } catch (const std::exception&) {
                }
            }
            return;
        }
        Req batch;
        for (const std::uint32_t id : mine) {
            const FileProgress& p = progress_[id];
            if (p.got < files_[id].size) {
                batch.ranges.push_back({.file_id = id, .offset = p.got, .length = files_[id].size - p.got});
            }
        }
        if (!batch.ranges.empty()) {
            send_message(link.sock, batch);
        }
        while (!mine.empty()) {
            receive_rest(link, mine.front(), received, buf);
            finish_file(mine.front());
            mine.erase(mine.begin());
        }
    }
}

// Receives the rest of file `id` (the next range the server sends, unless
// nothing is missing), then checks it and renames it into place. DATA
// payloads are written as they arrive, not frame by frame, so a connection
// cut mid-frame keeps every byte that got through.
void Download::receive_rest(Link& link, std::uint32_t id, std::uint64_t& received,
                            std::vector<std::byte>& buf) {
    FileProgress& p = progress_[id];
    const FileEntry& entry = files_[id];
    const fs::path part = temp_path(dir_, id);
    if (!p.part) {
        p.part = File::open_write(part);
        p.part.truncate(p.got);  // p.got is 0 here: drops a stale temp file from an earlier run
    }
    while (p.got < entry.size) {
        const auto header = read_frame_header(link.sock);
        if (!header) {
            throw ConnectionClosed("the server closed the connection");
        }
        if (header->type != static_cast<std::uint32_t>(MsgType::Data)) {
            const Message msg = link.reader.read_payload(link.sock, *header);
            if (const auto* err = std::get_if<Err>(&msg)) {
                throw ServerError(err->code, err->reason);
            }
            protocol_error(std::format("expected DATA for {}", printable(entry.name)));
        }
        if (header->length <= data_header_len || header->length > data_header_len + max_data_len) {
            protocol_error(std::format("DATA frame of {} bytes", header->length));
        }
        std::array<std::byte, data_header_len> head{};
        if (!link.sock.recv_all(head)) {
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
            const std::size_t n = link.sock.recv_some(std::span{buf}.first(std::min(left, buf.size())));
            if (n == 0) {
                throw ConnectionClosed(std::format("connection closed {} bytes into a DATA frame",
                                                   header->length - data_header_len - left));
            }
            const auto piece = std::span{buf}.first(n);
            p.part.write_at(piece, p.got);
            p.crc = crc32_update(p.crc, piece);
            p.got += n;
            received += n;
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
    fs::rename(part, dir_ / entry.name);
}

// Waits for work and claims a batch of it: up to max_req_ranges files, fewer
// when that leaves something for the other connections. Empty when everything
// is done (send_ack is set for the one connection that should say so) or the
// download was aborted.
std::vector<std::uint32_t> Download::claim(bool& send_ack) {
    std::unique_lock lock{mu_};
    cv_.wait(lock, [this] { return aborted_ || !todo_.empty() || done_ == files_.size(); });
    if (aborted_ || todo_.empty()) {
        send_ack = !aborted_ && !std::exchange(ack_claimed_, true);
        return {};
    }
    const std::size_t share = (todo_.size() + req_.connections - 1) / req_.connections;
    const std::size_t n = std::clamp<std::size_t>(share, 1, max_req_ranges);
    std::vector<std::uint32_t> batch;
    for (std::size_t i = 0; i < n && !todo_.empty(); ++i) {
        batch.push_back(todo_.front());
        todo_.pop_front();
        progress_[batch.back()].state = FileState::Busy;
    }
    return batch;
}

// A lost connection hands its unfinished files back, to the front of the
// queue so they are picked up next.
void Download::give_back(std::vector<std::uint32_t>& ids) {
    if (ids.empty()) {
        return;
    }
    {
        const std::lock_guard lock{mu_};
        for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
            progress_[*it].state = FileState::Todo;
            todo_.push_front(*it);
        }
    }
    ids.clear();
    cv_.notify_all();
}

void Download::finish_file(std::uint32_t id) {
    {
        const std::lock_guard lock{mu_};
        progress_[id].state = FileState::Done;
        ++done_;
    }
    cv_.notify_all();
}

// Stops every connection: shuts their sockets so none stays blocked.
void Download::abort(std::exception_ptr fatal, bool session_lost) {
    {
        const std::lock_guard lock{mu_};
        if (fatal && !fatal_) {
            fatal_ = std::move(fatal);
        }
        session_lost_ = session_lost_ || session_lost;
        aborted_ = true;
        for (Socket* sock : live_) {
            sock->shutdown();
        }
    }
    cv_.notify_all();
}

bool Download::is_aborted() {
    const std::lock_guard lock{mu_};
    return aborted_;
}

bool Download::all_done() {
    const std::lock_guard lock{mu_};
    return done_ == files_.size();
}

// Sleeps before a reconnect; wakes early if the download was aborted (returns
// false) or finished meanwhile (the caller then sees all_done()).
bool Download::sleep_unless_aborted(std::chrono::milliseconds wait) {
    std::unique_lock lock{mu_};
    cv_.wait_for(lock, wait, [this] { return aborted_ || done_ == files_.size(); });
    return !aborted_;
}

void Download::track(Socket* sock, bool live) {
    const std::lock_guard lock{mu_};
    if (live) {
        live_.push_back(sock);
        if (aborted_) {
            sock->shutdown();  // aborted while it was connecting
        }
    } else {
        std::erase(live_, sock);
    }
}

void Download::reset() {
    session_ = 0;
    files_.clear();
    progress_.clear();
    todo_.clear();
    done_ = 0;
    live_.clear();
    aborted_ = false;
    session_lost_ = false;
    ack_claimed_ = false;
    last_error_ = nullptr;
}

}  // namespace

ServerError::ServerError(ErrCode c, const std::string& reason)
    : std::runtime_error{std::format("server refused: {}: {}", to_string(c), printable(reason))},
      code{c} {}

FetchStats fetch(const FetchRequest& req) {
    if (!is_valid_topic(req.topic)) {
        throw std::invalid_argument(std::format("invalid topic name '{}'", printable(req.topic)));
    }
    if (req.connections == 0) {
        throw std::invalid_argument("connections must be at least 1");
    }
    Download download{req};
    return download.run();
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
