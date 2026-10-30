#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

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
    [[nodiscard]] ServerStats stats() const { return server_.stats(); }

private:
    Server server_;
    std::jthread thread_;
};

// Accepts `connections` connections and runs `script` on each (with its
// index, from 0, in accept order), each on its own thread so parallel clients
// work: a server that misbehaves on purpose.
class FakeServer {
public:
    explicit FakeServer(std::function<void(Socket&, int)> script, int connections)
        : listener_{Listener::listen("127.0.0.1", 0)},
          thread_{[this, script = std::move(script), connections] {
              std::vector<std::jthread> handlers;
              for (int i = 0; i < connections; ++i) {
                  Socket sock;
                  try {
                      sock = listener_.accept();
                  } catch (const std::exception&) {
                      break;
                  }
                  handlers.emplace_back([&script, i, s = std::move(sock)]() mutable {
                      try {
                          script(s, i);
                      } catch (const std::exception&) {  // the client hung up first
                      }
                  });
              }
          }} {}

    explicit FakeServer(std::function<void(Socket&)> script)
        : FakeServer([s = std::move(script)](Socket& sock, int) { s(sock); }, 1) {}
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return listener_.local_port(); }

private:
    Listener listener_;
    std::jthread thread_;
};

}  // namespace axon::test
