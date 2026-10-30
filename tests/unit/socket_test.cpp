#include <sys/socket.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "axon/net/socket.hpp"
#include "test_util.hpp"

namespace {

using axon::Listener;
using axon::Socket;

class SocketTest : public axon::test::NoFdLeak {};

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span{s.data(), s.size()});
}

std::string_view text(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

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

TEST_F(SocketTest, SendAllGathersHeadAndBody) {
    auto [a, b] = Socket::pair();
    a.send_all(bytes("head:"), bytes("body"));
    std::array<std::byte, 9> buf{};
    ASSERT_TRUE(b.recv_all(buf));
    EXPECT_EQ(text(buf), "head:body");
}

TEST_F(SocketTest, RecvAllReturnsFalseWhenPeerClosesCleanly) {
    auto [a, b] = Socket::pair();
    a = Socket{};
    std::array<std::byte, 4> buf{};
    EXPECT_FALSE(b.recv_all(buf));
}

TEST_F(SocketTest, RecvAllThrowsWhenPeerClosesPartWay) {
    auto [a, b] = Socket::pair();
    a.send_all(bytes("ab"));
    a = Socket{};
    std::array<std::byte, 4> buf{};
    EXPECT_THROW((void)b.recv_all(buf), axon::ConnectionClosed);
}

TEST_F(SocketTest, SendToClosedPeerThrowsInsteadOfRaisingSigpipe) {
    auto [a, b] = Socket::pair();
    b = Socket{};
    try {
        a.send_all(bytes("x"));
        FAIL() << "send should have failed";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), std::errc::broken_pipe) << e.what();
    }
}

TEST_F(SocketTest, LargeTransferOverTcpArrivesIntact) {
    const auto listener = Listener::listen("127.0.0.1", 0);
    auto client = Socket::connect("127.0.0.1", listener.local_port());
    auto server = listener.accept();
    std::vector<std::byte> sent(4 << 20);  // far more than the socket buffers hold
    for (std::size_t i = 0; i < sent.size(); ++i) {
        sent[i] = static_cast<std::byte>(i * 31 % 251);
    }
    std::jthread sender{[&client, &sent] { client.send_all(sent); }};
    std::vector<std::byte> got(sent.size());
    ASSERT_TRUE(server.recv_all(got));
    EXPECT_EQ(got, sent);
}

TEST_F(SocketTest, TimeoutTurnsASilentPeerIntoAnError) {
    using namespace std::chrono_literals;
    auto [a, b] = Socket::pair();
    b.set_timeouts(100ms);
    std::array<std::byte, 1> buf{};
    try {
        (void)b.recv_all(buf);
        FAIL() << "recv should have timed out";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), std::errc::timed_out) << e.what();
    }
}

TEST_F(SocketTest, ShutdownWakesAThreadBlockedInRecv) {
    auto [a, b] = Socket::pair();
    Socket& rx = b;
    bool got_data = true;
    std::jthread reader{[&rx, &got_data] {
        std::array<std::byte, 1> buf{};
        got_data = rx.recv_all(buf);
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});  // let it block
    b.shutdown();
    reader.join();
    EXPECT_FALSE(got_data);
}

TEST_F(SocketTest, TryAcceptDoesNotWaitWhenNobodyConnects) {
    const auto listener = Listener::listen("127.0.0.1", 0);
    EXPECT_FALSE(listener.try_accept().has_value());
    const auto client = Socket::connect("127.0.0.1", listener.local_port());
    std::optional<Socket> server;
    for (int i = 0; i < 100 && !server; ++i) {  // the handshake completes asynchronously
        server = listener.try_accept();
        if (!server) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    EXPECT_TRUE(server.has_value());
}

}  // namespace
