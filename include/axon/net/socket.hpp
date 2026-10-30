#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "axon/fd.hpp"

namespace axon {

// The peer closed the connection in the middle of something we were reading.
class ConnectionClosed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A connected stream socket. Closes itself when destroyed; move-only.
// I/O errors (reset, timeout, writing to a closed peer) throw std::system_error.
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

    // Sends all of `head` then all of `body`, in one system call when the
    // kernel takes it all. A closed peer gives EPIPE, never SIGPIPE.
    void send_all(std::span<const std::byte> head, std::span<const std::byte> body = {});

    // Fills `buf` completely. Returns false if the peer closed before sending
    // a single byte; throws ConnectionClosed if it closed part-way through.
    [[nodiscard]] bool recv_all(std::span<std::byte> buf);

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
