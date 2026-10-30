#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <thread>

#include "axon/fd.hpp"
#include "axon/net/socket.hpp"
#include "axon/storage/dataset.hpp"

namespace axon {

struct ServerConfig {
    // A client that sends nothing (or stops reading) for this long is dropped.
    std::chrono::milliseconds idle_timeout{std::chrono::seconds{60}};
};

// Serves a Dataset over TCP with one thread per connection.
class Server {
public:
    Server(Dataset dataset, Listener listener, ServerConfig config = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Accepts and serves clients until stop(), then closes every connection
    // and joins its thread before returning.
    void run();

    // Makes run() return. Async-signal-safe, so a signal handler may call it.
    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const { return listener_.local_port(); }

private:
    struct Conn {
        std::uint64_t id = 0;
        Socket sock;  // stays open until the thread is joined, so shutdown() never hits a reused fd
        std::atomic<bool> done{false};
        std::jthread thread;  // declared last: joined before the socket closes
    };

    void spawn(Socket sock);
    void serve(Conn& conn);  // runs on the connection's thread
    void reap();             // joins connections whose thread has finished
    void close_all() noexcept;

    Dataset dataset_;
    Listener listener_;
    ServerConfig config_;
    Fd wake_;  // eventfd that stop() writes to wake poll()
    std::atomic<bool> stopping_{false};
    std::mutex mu_;          // guards conns_ and next_id_
    std::list<Conn> conns_;  // a list, so a Conn never moves while its thread runs
    std::uint64_t next_id_ = 1;
};

}  // namespace axon
