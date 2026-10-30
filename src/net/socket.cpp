#include "axon/net/socket.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <system_error>

#include "net/iov.hpp"
#include "sys_error.hpp"

namespace axon {

Socket Socket::connect(const std::string& host, std::uint16_t port) {
    const std::string service = std::to_string(port);
    const std::string where = host + ":" + service;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &found); rc != 0) {
        throw std::runtime_error("resolve " + host + ": " + ::gai_strerror(rc));
    }
    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> list{found, &::freeaddrinfo};

    int err = ECONNREFUSED;
    for (const addrinfo* ai = list.get(); ai != nullptr; ai = ai->ai_next) {
        Fd fd{::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol)};
        if (fd && ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen) == 0) {
            return Socket{std::move(fd)};
        }
        err = errno;  // read before fd's destructor runs close()
    }
    throw std::system_error(err, std::generic_category(), "connect " + where);
}

std::pair<Socket, Socket> Socket::pair() {
    std::array<int, 2> fds{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) != 0) {
        detail::throw_errno("socketpair");
    }
    return {Socket{Fd{fds[0]}}, Socket{Fd{fds[1]}}};
}

void Socket::send_all(std::span<const std::byte> head, std::span<const std::byte> body) {
    // sendmsg never writes through iov_base, so dropping const is safe.
    std::array<iovec, 2> iov{
        iovec{const_cast<std::byte*>(head.data()), head.size()},
        iovec{const_cast<std::byte*>(body.data()), body.size()},
    };
    std::size_t first = detail::skip_written(iov, 0, 0);  // first iovec with bytes left
    while (first < iov.size()) {
        msghdr msg{};
        msg.msg_iov = &iov[first];
        msg.msg_iovlen = iov.size() - first;
        const ssize_t n = ::sendmsg(fd(), &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            detail::throw_errno("send");
        }
        first = detail::skip_written(iov, first, static_cast<std::size_t>(n));
    }
}

bool Socket::recv_all(std::span<std::byte> buf) {
    std::size_t done = 0;
    while (done < buf.size()) {
        const ssize_t n = ::recv(fd(), buf.data() + done, buf.size() - done, 0);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
        } else if (n == 0) {
            if (done == 0) {
                return false;
            }
            throw ConnectionClosed("connection closed after " + std::to_string(done) + " of " +
                                   std::to_string(buf.size()) + " bytes");
        } else if (errno != EINTR) {
            detail::throw_errno("recv");
        }
    }
    return true;
}

Listener Listener::listen(const std::string& address, std::uint16_t port) {
    const std::string where = address + ":" + std::to_string(port);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        throw std::invalid_argument("not an IPv4 address: " + address);
    }

    Fd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd) {
        detail::throw_errno("socket");
    }
    // Lets a restarted server bind its port again right away.
    const int one = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        detail::throw_errno("setsockopt SO_REUSEADDR");
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        detail::throw_errno("bind", where);
    }
    if (::listen(fd.get(), SOMAXCONN) != 0) {
        detail::throw_errno("listen", where);
    }
    return Listener{std::move(fd)};
}

Socket Listener::accept() const {
    for (;;) {
        Fd fd{::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (fd) {
            return Socket{std::move(fd)};
        }
        // The client gave up before we accepted it: wait for the next one.
        if (errno != EINTR && errno != ECONNABORTED) {
            detail::throw_errno("accept");
        }
    }
}

std::uint16_t Listener::local_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        detail::throw_errno("getsockname");
    }
    return ntohs(addr.sin_port);
}

}  // namespace axon
