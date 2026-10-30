#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "axon/net/socket.hpp"
#include "axon/protocol/bytes.hpp"
#include "axon/protocol/frame.hpp"
#include "axon/storage/dataset.hpp"
#include "axon/transfer/server.hpp"
#include "test_servers.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using namespace axon::proto;
using namespace std::chrono_literals;
using axon::Dataset;
using axon::ServerConfig;
using axon::Socket;
using axon::test::write_file;

std::string pattern(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>('a' + i % 26);
    }
    return s;
}

// A server on a free loopback port, run on its own thread.
class ServerTest : public axon::test::TempDirTest {
protected:
    const std::string big = pattern(150'000);  // spans several DATA frames

    void SetUp() override {
        TempDirTest::SetUp();
        write_file(dir_ / "animals" / "big.bin", big);
        write_file(dir_ / "animals" / "cat.txt", "meow");
        write_file(dir_ / "animals" / "empty.txt", "");
    }

    void start(ServerConfig config = {.idle_timeout = 5s}) { server_.emplace(dir_, config); }

    void TearDown() override {
        server_.reset();
        TempDirTest::TearDown();  // also checks the server closed every descriptor
    }

    Socket connect() const { return Socket::connect("127.0.0.1", server_->port()); }

    std::optional<axon::test::LiveServer> server_;
};

Message ask(Socket& sock, MessageReader& reader, const Message& msg) {
    send_message(sock, msg);
    auto reply = reader.read(sock);
    if (!reply) {
        throw std::runtime_error("server closed without replying");
    }
    return std::move(*reply);
}

Err expect_err(Socket& sock, const Message& msg) {
    MessageReader reader;
    const auto reply = ask(sock, reader, msg);
    if (!std::holds_alternative<Err>(reply)) {
        ADD_FAILURE() << "expected ERR, got type " << static_cast<std::uint32_t>(type_of(reply));
        return {};
    }
    EXPECT_FALSE(reader.read(sock).has_value()) << "server should close after ERR";
    return std::get<Err>(reply);
}

// Collects DATA frames until `length` bytes of file `id` from `offset` arrived.
std::string receive(Socket& sock, MessageReader& reader, std::uint32_t id, std::uint64_t offset,
                    std::uint64_t length) {
    std::string out;
    while (out.size() < length) {
        const auto msg = reader.read(sock);
        if (!msg || !std::holds_alternative<Data>(*msg)) {
            ADD_FAILURE() << "expected DATA";
            return out;
        }
        const auto& d = std::get<Data>(*msg);
        EXPECT_EQ(d.file_id, id);
        EXPECT_EQ(d.offset, offset + out.size());
        out.append(reinterpret_cast<const char*>(d.bytes.data()), d.bytes.size());
    }
    return out;
}

// What one connection delivered before it ended.
struct Delivery {
    std::string bytes;           // file bytes of complete DATA frames, in order
    bool cut_mid_frame = false;  // the stream ended inside a frame
};

// Reads DATA frames until `length` bytes arrived or the connection ends.
Delivery receive_until_cut(Socket& sock, MessageReader& reader, std::uint64_t length) {
    Delivery d;
    try {
        while (d.bytes.size() < length) {
            const auto msg = reader.read(sock);
            if (!msg) {
                break;
            }
            const auto& data = std::get<Data>(*msg);
            d.bytes.append(reinterpret_cast<const char*>(data.bytes.data()), data.bytes.size());
        }
    } catch (const axon::ConnectionClosed&) {
        d.cut_mid_frame = true;
    }
    return d;
}

// --- the happy path ---

TEST_F(ServerTest, ServesListAndRangesUntilAck) {
    start();
    auto sock = connect();
    MessageReader reader;
    const auto ok = std::get<Ok>(ask(sock, reader, Get{.topic = "animals"}));
    EXPECT_EQ(ok.files, Dataset::scan(dir_).find("animals")->files);
    ASSERT_EQ(ok.files->size(), 3u);
    ASSERT_EQ((*ok.files)[0].name, "big.bin");

    send_message(sock, Req{{{.file_id = 0, .offset = 0, .length = big.size()}}});
    EXPECT_EQ(receive(sock, reader, 0, 0, big.size()), big);

    // Two ranges in one REQ arrive in order; a range may start mid-file.
    send_message(sock, Req{{{.file_id = 1, .offset = 1, .length = 3},
                            {.file_id = 0, .offset = 70'000, .length = 5}}});
    EXPECT_EQ(receive(sock, reader, 1, 1, 3), "eow");
    EXPECT_EQ(receive(sock, reader, 0, 70'000, 5), big.substr(70'000, 5));

    send_message(sock, Ack{});
    EXPECT_FALSE(reader.read(sock).has_value()) << "server should close after ACK";
}

// --- sessions ---

TEST_F(ServerTest, EachDownloadGetsItsOwnSession) {
    start();
    auto a = connect();
    auto b = connect();
    MessageReader ra;
    MessageReader rb;
    const auto ok_a = std::get<Ok>(ask(a, ra, Get{.topic = "animals"}));
    const auto ok_b = std::get<Ok>(ask(b, rb, Get{.topic = "animals"}));
    EXPECT_NE(ok_a.session_id, 0u);
    EXPECT_NE(ok_a.session_id, ok_b.session_id);
}

TEST_F(ServerTest, ResumesAtTheExactByteAfterACut) {
    start();
    std::uint64_t session = 0;
    std::string got;
    {
        auto sock = connect();
        MessageReader reader;
        session = std::get<Ok>(ask(sock, reader, Get{.topic = "animals"})).session_id;
        send_message(sock, Req{{{.file_id = 0, .offset = 0, .length = big.size()}}});
        got = receive(sock, reader, 0, 0, 2 * max_data_len);  // two frames, then the line drops
    }

    auto sock = connect();
    MessageReader reader;
    const auto ok = std::get<Ok>(ask(sock, reader, Get{.topic = "animals", .session_id = session}));
    EXPECT_EQ(ok.session_id, session);
    EXPECT_FALSE(ok.files.has_value()) << "the client already has the list";
    const std::uint64_t at = got.size();
    send_message(sock, Req{{{.file_id = 0, .offset = at, .length = big.size() - at}}});
    got += receive(sock, reader, 0, at, big.size() - at);
    EXPECT_EQ(got, big);
    send_message(sock, Ack{});
    EXPECT_FALSE(reader.read(sock).has_value());

    server_->stop();
    const auto stats = server_->stats();
    EXPECT_EQ(stats.sessions_finished, 1u);
    // Bytes in flight at the cut were sent twice; every byte still reached
    // the client exactly once.
    EXPECT_EQ(stats.payload_bytes - stats.redundant_bytes, big.size());
}

TEST_F(ServerTest, ResumeCanAskForTheListAgain) {
    start();
    auto first = connect();
    MessageReader r1;
    const auto ok = std::get<Ok>(ask(first, r1, Get{.topic = "animals"}));
    auto again = connect();
    MessageReader r2;
    const auto resumed = std::get<Ok>(
        ask(again, r2, Get{.topic = "animals", .session_id = ok.session_id, .want_list = true}));
    EXPECT_EQ(resumed.files, ok.files);
}

TEST_F(ServerTest, SessionBelongsToOneTopic) {
    write_file(dir_ / "plants" / "fern.txt", "green");
    start();
    auto first = connect();
    MessageReader reader;
    const auto ok = std::get<Ok>(ask(first, reader, Get{.topic = "animals"}));
    auto other = connect();
    EXPECT_EQ(expect_err(other, Get{.topic = "plants", .session_id = ok.session_id}).code,
              ErrCode::TopicMismatch);
}

TEST_F(ServerTest, AckEndsTheSession) {
    start();
    std::uint64_t session = 0;
    {
        auto sock = connect();
        MessageReader reader;
        session = std::get<Ok>(ask(sock, reader, Get{.topic = "animals"})).session_id;
        send_message(sock, Ack{});
        EXPECT_FALSE(reader.read(sock).has_value());
    }
    auto sock = connect();
    EXPECT_EQ(expect_err(sock, Get{.topic = "animals", .session_id = session}).code,
              ErrCode::SessionNotFound);
}

TEST_F(ServerTest, AbandonedSessionExpires) {
    start({.idle_timeout = 5s, .session_ttl = 100ms});
    std::uint64_t session = 0;
    {
        auto sock = connect();
        MessageReader reader;
        session = std::get<Ok>(ask(sock, reader, Get{.topic = "animals"})).session_id;
    }  // the client gives up
    std::this_thread::sleep_for(300ms);
    auto sock = connect();
    EXPECT_EQ(expect_err(sock, Get{.topic = "animals", .session_id = session}).code,
              ErrCode::SessionNotFound);
}

// --- fault injection ---

TEST_F(ServerTest, FaultInjectionCutsInsideADataFrame) {
    start({.idle_timeout = 5s, .fault = {.probability = 1, .seed = 7}});
    auto sock = connect();
    MessageReader reader;
    (void)ask(sock, reader, Get{.topic = "animals"});
    // Ask for every byte of the topic, so the cut point (drawn over all of it) is inside.
    send_message(sock, Req{{{.file_id = 0, .offset = 0, .length = big.size()},
                            {.file_id = 1, .offset = 0, .length = 4}}});
    const Delivery d = receive_until_cut(sock, reader, big.size() + 4);
    EXPECT_TRUE(d.cut_mid_frame) << "the stream should end inside a DATA frame";
    EXPECT_LT(d.bytes.size(), big.size() + 4);
}

TEST_F(ServerTest, CutPointIsDrawnOverOneConnectionsShare) {
    // Like the C version: with 4 connections each carries about a quarter, so
    // the cut lands in the first quarter of what is left.
    start({.idle_timeout = 5s, .fault = {.probability = 1, .seed = 7}});
    const std::uint64_t total = big.size() + 4;
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto sock = connect();
        MessageReader reader;
        (void)ask(sock, reader, Get{.topic = "animals", .connections = 4});
        send_message(sock, Req{{{.file_id = 0, .offset = 0, .length = big.size()},
                                {.file_id = 1, .offset = 0, .length = 4}}});
        const Delivery d = receive_until_cut(sock, reader, total);
        EXPECT_TRUE(d.cut_mid_frame);
        EXPECT_LE(d.bytes.size(), total / 4);
    }
}

TEST_F(ServerTest, ListIsNeverCut) {
    start({.idle_timeout = 5s, .fault = {.probability = 1, .seed = 7}});
    auto sock = connect();
    MessageReader reader;
    EXPECT_TRUE(std::holds_alternative<Topics>(ask(sock, reader, List{})));
}

TEST_F(ServerTest, ResumingThroughRepeatedCutsDeliversEveryByteOnce) {
    start({.idle_timeout = 5s, .fault = {.probability = 0.8, .seed = 11}});
    std::string got;
    std::uint64_t session = 0;
    int attempts = 0;
    int cuts = 0;
    while (got.size() < big.size()) {
        ASSERT_LT(++attempts, 50) << "no progress";
        auto sock = connect();
        MessageReader reader;
        session = std::get<Ok>(ask(sock, reader, Get{.topic = "animals", .session_id = session}))
                      .session_id;
        const std::uint64_t at = got.size();
        send_message(sock, Req{{{.file_id = 0, .offset = at, .length = big.size() - at}}});
        const Delivery d = receive_until_cut(sock, reader, big.size() - at);
        got += d.bytes;
        cuts += d.cut_mid_frame ? 1 : 0;
        if (!d.cut_mid_frame && got.size() == big.size()) {
            send_message(sock, Ack{});
            EXPECT_FALSE(reader.read(sock).has_value());
        }
    }
    EXPECT_EQ(got, big);
    EXPECT_GE(cuts, 2) << "with p = 0.8 several connections should have been cut";

    server_->stop();
    const auto stats = server_->stats();
    EXPECT_EQ(stats.sessions_finished, 1u);
    EXPECT_EQ(stats.payload_bytes - stats.redundant_bytes, big.size());
}

TEST_F(ServerTest, ServesManyClientsAtOnce) {
    start();
    std::vector<std::string> got(8);
    {
        std::vector<std::jthread> clients;
        for (auto& out : got) {
            clients.emplace_back([this, &out] {
                auto sock = connect();
                MessageReader reader;
                (void)ask(sock, reader, Get{.topic = "animals"});
                send_message(sock, Req{{{.file_id = 0, .offset = 0, .length = big.size()}}});
                out = receive(sock, reader, 0, 0, big.size());
                send_message(sock, Ack{});
            });
        }
    }
    for (const auto& out : got) {
        EXPECT_EQ(out, big);
    }
}

TEST_F(ServerTest, ListsTopicsWithFileCountsAndSizes) {
    write_file(dir_ / "plants" / "fern.txt", "green");
    start();
    auto sock = connect();
    MessageReader reader;
    const auto topics = std::get<Topics>(ask(sock, reader, List{}));
    const Topics expected{{{"animals", 3, big.size() + 4}, {"plants", 1, 5}}};
    EXPECT_EQ(topics, expected);
    EXPECT_FALSE(reader.read(sock).has_value()) << "server should close after TOPS";
}

// --- refusals ---

TEST_F(ServerTest, RefusesListFromOtherProtocolVersion) {
    start();
    auto sock = connect();
    EXPECT_EQ(expect_err(sock, List{.version = 2}).code, ErrCode::BadVersion);
}

TEST_F(ServerTest, RefusesUnknownTopic) {
    start();
    auto sock = connect();
    EXPECT_EQ(expect_err(sock, Get{.topic = "zebras"}).code, ErrCode::UnknownTopic);
}

TEST_F(ServerTest, RefusesOtherProtocolVersion) {
    start();
    auto sock = connect();
    EXPECT_EQ(expect_err(sock, Get{.version = 99, .topic = "animals"}).code, ErrCode::BadVersion);
}

TEST_F(ServerTest, RefusesSessionResume) {
    start();
    auto sock = connect();
    EXPECT_EQ(expect_err(sock, Get{.topic = "animals", .session_id = 7}).code,
              ErrCode::SessionNotFound);
}

TEST_F(ServerTest, FirstMessageMustBeGet) {
    start();
    auto sock = connect();
    EXPECT_EQ(expect_err(sock, Ack{}).code, ErrCode::BadRequest);
}

TEST_F(ServerTest, MalformedFrameGetsBadRequest) {
    start();
    auto sock = connect();
    std::array<std::byte, frame_header_len> header{};
    axon::store_be(std::span{header}.first<4>(), tag("NOPE"));
    sock.send_all(header);
    MessageReader reader;
    const auto reply = reader.read(sock);
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(std::holds_alternative<Err>(*reply));
    EXPECT_EQ(std::get<Err>(*reply).code, ErrCode::BadRequest);
}

TEST_F(ServerTest, RefusesRangesOutsideTheFiles) {
    start();
    for (const Range bad : {Range{.file_id = 9, .offset = 0, .length = 1},
                            Range{.file_id = 1, .offset = 0, .length = 5},     // cat.txt is 4 bytes
                            Range{.file_id = 2, .offset = 0, .length = 1}}) {  // empty.txt
        auto sock = connect();
        MessageReader reader;
        (void)ask(sock, reader, Get{.topic = "animals"});
        send_message(sock, Req{{bad}});
        const auto reply = reader.read(sock);
        ASSERT_TRUE(reply.has_value());
        ASSERT_TRUE(std::holds_alternative<Err>(*reply)) << "file " << bad.file_id;
        EXPECT_EQ(std::get<Err>(*reply).code, ErrCode::BadRange);
    }
}

TEST_F(ServerTest, ReportsFileThatShrankSinceTheScan) {
    start();
    fs::resize_file(dir_ / "animals" / "big.bin", 10);
    auto sock = connect();
    MessageReader reader;
    (void)ask(sock, reader, Get{.topic = "animals"});
    send_message(sock, Req{{{.file_id = 0, .offset = 0, .length = big.size()}}});
    (void)receive(sock, reader, 0, 0, 10);  // what is left is still sent
    const auto reply = reader.read(sock);
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(std::holds_alternative<Err>(*reply));
    EXPECT_EQ(std::get<Err>(*reply).code, ErrCode::Internal);
}

// --- lifecycle ---

TEST_F(ServerTest, StopClosesConnectedClientsPromptly) {
    start();
    auto idle = connect();  // connected, never sends anything
    auto mid = connect();   // stopped mid-session
    MessageReader reader;
    (void)ask(mid, reader, Get{.topic = "animals"});

    const auto t0 = std::chrono::steady_clock::now();
    server_->stop();  // returns once run() has joined every connection thread
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);

    // `idle` may not have been accepted yet when stop() came (the kernel can
    // queue the two handshakes in either order): then the closed listener
    // resets it. Either way it must end, not hang.
    const auto ended = [](Socket& sock) {
        try {
            MessageReader r;
            return !r.read(sock).has_value();
        } catch (const axon::NetworkError&) {
            return true;
        }
    };
    EXPECT_TRUE(ended(idle));
    EXPECT_FALSE(reader.read(mid).has_value());
    server_.reset();
}

TEST_F(ServerTest, ClientsStillQueuedAtStopAreRefusedNotLeftHanging) {
    start();
    server_->stop();
    // The server is still alive but stopped: connecting must fail, not queue.
    EXPECT_THROW((void)connect(), axon::NetworkError);
}

TEST_F(ServerTest, DropsClientThatStaysSilent) {
    start({.idle_timeout = 200ms});
    auto sock = connect();
    const auto t0 = std::chrono::steady_clock::now();
    MessageReader reader;
    EXPECT_FALSE(reader.read(sock).has_value());  // the server hangs up on us
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 150ms);
}

}  // namespace
