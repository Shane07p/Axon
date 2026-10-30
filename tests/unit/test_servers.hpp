#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>

#include "axon/net/socket.hpp"
#include "axon/storage/dataset.hpp"
#include "axon/transfer/server.hpp"

namespace axon::test {

// A real Server over `root` on a free loopback port, running on its own thread.
class LiveServer {
public:
    explicit LiveServer(const std::filesystem::path& root,
                        ServerConfig config = {.idle_timeout = std::chrono::seconds{5}})
        : server_{Dataset::scan(root), Listener::listen("127.0.0.1", 0), config},
          thread_{[this] { server_.run(); }} {}
    ~LiveServer() { stop(); }
    LiveServer(const LiveServer&) = delete;
    LiveServer& operator=(const LiveServer&) = delete;

    // Stops the server and waits until run() has returned.
    void stop() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const { return server_.port(); }

private:
    Server server_;
    std::jthread thread_;
};

// Accepts one connection and runs `script` on it, on its own thread: a server
// that misbehaves on purpose.
class FakeServer {
public:
    explicit FakeServer(std::function<void(Socket&)> script)
        : listener_{Listener::listen("127.0.0.1", 0)},
          thread_{[this, script = std::move(script)] {
              try {
                  Socket sock = listener_.accept();
                  script(sock);
              } catch (const std::exception&) {  // the client hung up first
              }
          }} {}
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return listener_.local_port(); }

private:
    Listener listener_;
    std::jthread thread_;
};

}  // namespace axon::test
