#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "axon/fd.hpp"

namespace axon {

// A connected stream socket. Closes itself when destroyed; move-only.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(Fd fd) noexcept : fd_{std::move(fd)} {}

    // Connects over TCP, trying each address `host` resolves to (IPv4 or IPv6).
    // Throws std::system_error if none accepts, std::runtime_error if `host`
    // does not resolve.
    [[nodiscard]] static Socket connect(const std::string& host, std::uint16_t port);

    // Two sockets connected to each other, for tests.
    [[nodiscard]] static std::pair<Socket, Socket> pair();

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(fd_); }

private:
    Fd fd_;
};

// A TCP socket that accepts incoming connections.
class Listener {
public:
    Listener() noexcept = default;
    explicit Listener(Fd fd) noexcept : fd_{std::move(fd)} {}

    // Binds an IPv4 address ("0.0.0.0" = all interfaces) and starts listening.
    // Port 0 picks a free port; read it back with local_port().
    [[nodiscard]] static Listener listen(const std::string& address, std::uint16_t port);

    // Blocks until a client connects.
    [[nodiscard]] Socket accept() const;

    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

private:
    Fd fd_;
};

}  // namespace axon
