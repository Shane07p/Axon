#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "axon/net/socket.hpp"
#include "test_util.hpp"

namespace {

using axon::Listener;
using axon::Socket;

class SocketTest : public axon::test::NoFdLeak {};

// Sends `msg` on one socket and checks it arrives intact on the other.
void expect_delivers(const Socket& from, const Socket& to, std::string_view msg) {
    const auto len = static_cast<ssize_t>(msg.size());
    ASSERT_EQ(::send(from.fd(), msg.data(), msg.size(), MSG_NOSIGNAL), len);
    std::array<char, 64> buf{};
    ASSERT_EQ(::recv(to.fd(), buf.data(), msg.size(), MSG_WAITALL), len);
    EXPECT_EQ(std::string_view(buf.data(), msg.size()), msg);
}

TEST_F(SocketTest, ConnectAndAcceptOnLoopback) {
    const auto listener = Listener::listen("127.0.0.1", 0);
    ASSERT_NE(listener.local_port(), 0);
    const auto client = Socket::connect("127.0.0.1", listener.local_port());
    const auto server = listener.accept();
    expect_delivers(client, server, "ping");
    expect_delivers(server, client, "pong");
}

TEST_F(SocketTest, ConnectResolvesHostName) {
    // "localhost" may resolve to ::1 first; connect must fall through to 127.0.0.1.
    const auto listener = Listener::listen("127.0.0.1", 0);
    const auto client = Socket::connect("localhost", listener.local_port());
    const auto server = listener.accept();
    expect_delivers(client, server, "hello");
}

TEST_F(SocketTest, ConnectToClosedPortIsRefused) {
    const std::uint16_t port = Listener::listen("127.0.0.1", 0).local_port();  // closed again
    try {
        (void)Socket::connect("127.0.0.1", port);
        FAIL() << "connect should have failed";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), std::errc::connection_refused) << e.what();
    }
}

TEST_F(SocketTest, ListenOnBusyPortFails) {
    const auto first = Listener::listen("127.0.0.1", 0);
    try {
        (void)Listener::listen("127.0.0.1", first.local_port());
        FAIL() << "second listen should have failed";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), std::errc::address_in_use) << e.what();
    }
}

TEST_F(SocketTest, ListenRejectsBadAddress) {
    EXPECT_THROW((void)Listener::listen("not-an-ip", 0), std::invalid_argument);
}

TEST_F(SocketTest, PairIsConnectedBothWays) {
    const auto [a, b] = Socket::pair();
    expect_delivers(a, b, "left");
    expect_delivers(b, a, "right");
}

}  // namespace
