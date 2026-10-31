#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "axon/fault/fault.hpp"
#include "axon/fd.hpp"
#include "axon/net/socket.hpp"
#include "axon/storage/dataset.hpp"
#include "axon/transfer/session.hpp"

namespace axon {

namespace detail {
class SessionWorker;
struct Stream;
}  // namespace detail

struct ServerConfig {
    // A client that sends nothing (or stops reading) for this long is dropped.
    std::chrono::milliseconds idle_timeout{std::chrono::seconds{60}};
    // A session no connection has used for this long can no longer be resumed.
    std::chrono::milliseconds session_ttl{std::chrono::minutes{10}};
    // For testing recovery: cut connections on purpose (probability 0 = never).
    FaultConfig fault{};
};

// Totals over sessions that finished (the client sent ACK).
struct ServerStats {
    std::uint64_t sessions_finished = 0;
    std::uint64_t payload_bytes = 0;    // file bytes sent
    std::uint64_t redundant_bytes = 0;  // of those, bytes the same session was sent before
};

// Serves a Dataset over TCP. Each new connection gets a short-lived thread
// for its handshake (GET or LIST); a download's connections are then served
// together by one thread per session, with poll() and non-blocking sockets.
class Server {
public:
    Server(Dataset dataset, Listener listener, ServerConfig config = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Accepts and serves clients until stop(). Then closes the listening
    // socket (clients still waiting to be accepted get a reset), closes every
    // connection and joins its thread before returning.
    void run();

    // Makes run() return. Async-signal-safe, so a signal handler may call it.
    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] ServerStats stats() const;

private:
    struct Conn {
        std::uint64_t id = 0;
        Socket sock;  // stays open until the thread is joined, so shutdown() never hits a reused fd
        std::atomic<bool> done{false};
        std::jthread thread;  // declared last: joined before the socket closes
    };

    void spawn(Socket sock);
    void serve(Conn& conn);  // runs on the connection's thread
    void handshake(Conn& conn, const std::string& who);
    void hand_off(const std::shared_ptr<Session>& session, detail::Stream stream);
    Socket take_socket(Conn& conn);
    void finish(const Session& session, const std::string& who);
    void reap();  // joins finished handshake threads and session workers
    void close_all() noexcept;

    Dataset dataset_;
    Listener listener_;
    std::uint16_t port_;
    ServerConfig config_;
    SessionTable sessions_;  // after dataset_: sessions point into it
    std::atomic<std::uint64_t> sessions_finished_{0};
    std::atomic<std::uint64_t> payload_bytes_{0};
    std::atomic<std::uint64_t> redundant_bytes_{0};
    Fd wake_;  // eventfd that stop() writes to wake poll()
    std::atomic<bool> stopping_{false};
    std::mutex mu_;          // guards conns_, next_id_ and each Conn's sock
    std::list<Conn> conns_;  // a list, so a Conn never moves while its thread runs
    std::uint64_t next_id_ = 1;
    std::mutex workers_mu_;  // guards workers_
    std::unordered_map<std::uint64_t, std::unique_ptr<detail::SessionWorker>> workers_;  // by session id
};

}  // namespace axon
