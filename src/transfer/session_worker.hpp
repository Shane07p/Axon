#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "axon/fault/fault.hpp"
#include "axon/fd.hpp"
#include "axon/net/socket.hpp"
#include "axon/transfer/session.hpp"

namespace axon::detail {

// A connection whose handshake is done, handed to its session's worker.
struct Stream {
    std::uint64_t id = 0;  // the server's connection number, for logs
    Socket sock;           // switched to non-blocking by the worker
    FaultPlan fault;
};

struct WorkerConfig {
    std::chrono::milliseconds idle_timeout{};
    // Called once, on the worker's thread, when the client sends ACK.
    std::function<void(const Session&, const std::string& who)> on_finish;
};

// Serves every connection of one session from a single thread with poll()
// and non-blocking sockets. Each connection has its own queue of requested
// ranges and its own send buffer, so one whose client reads slowly only
// delays itself. Exits once it has no connections left.
class SessionWorker {
public:
    // Starts with its first connection, so it never sees itself empty and
    // exits before anything is handed over.
    SessionWorker(std::shared_ptr<Session> session, WorkerConfig config, Stream first);
    ~SessionWorker();  // stops and joins
    SessionWorker(const SessionWorker&) = delete;
    SessionWorker& operator=(const SessionWorker&) = delete;

    // Hands a connection over. Returns false (and leaves `stream` alone) if
    // the worker is already exiting; the caller then starts a new one.
    bool add(Stream& stream);

    // Closes every connection and makes the thread return. Thread-safe.
    void stop() noexcept;

    [[nodiscard]] bool exited() const;

private:
    void run();
    void wake() noexcept;

    const std::shared_ptr<Session> session_;
    const WorkerConfig config_;
    Fd wake_;  // eventfd: a connection arrived, or stop() was called
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::vector<Stream> incoming_;  // guarded by mu_
    bool exiting_ = false;          // guarded by mu_
    std::jthread thread_;           // last: started once everything else is set up
};

}  // namespace axon::detail
