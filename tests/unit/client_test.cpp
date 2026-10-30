#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "axon/crc32.hpp"
#include "axon/net/socket.hpp"
#include "axon/protocol/frame.hpp"
#include "axon/transfer/client.hpp"
#include "test_servers.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using namespace axon::proto;
using namespace std::chrono_literals;
using axon::FetchStats;
using axon::Socket;
using axon::test::FakeServer;
using axon::test::LiveServer;
using axon::test::write_file;

std::string read_file(const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<std::string> listing(const fs::path& dir) {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator{dir}) {
        out.push_back(e.path().filename().string());
    }
    std::ranges::sort(out);
    return out;
}

std::uint32_t crc_of(std::string_view s) {
    return axon::crc32_update(0, std::as_bytes(std::span{s.data(), s.size()}));
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span{s.data(), s.size()});
}

class FetchTest : public axon::test::TempDirTest {
protected:
    fs::path data() const { return dir_ / "data"; }
    fs::path out() const { return dir_ / "out"; }

    FetchStats fetch_from(std::uint16_t port, const std::string& topic) const {
        return axon::fetch(
            {.host = "127.0.0.1", .port = port, .topic = topic, .output = out(), .timeout = 5s});
    }

    // Every file of data()/topic must be in out()/topic with the same bytes,
    // and nothing else (no leftover temporary files).
    void expect_identical(const std::string& topic) const {
        const auto want = listing(data() / topic);
        ASSERT_EQ(listing(out() / topic), want);
        for (const auto& name : want) {
            EXPECT_EQ(read_file(out() / topic / name), read_file(data() / topic / name)) << name;
        }
    }
};

// --- against the real server ---

TEST_F(FetchTest, DownloadsEveryFileByteForByte) {
    std::string binary(300'000, '\0');  // every byte value, over several DATA frames
    for (std::size_t i = 0; i < binary.size(); ++i) {
        binary[i] = static_cast<char>(i * 131 % 256);
    }
    write_file(data() / "animals" / "big.bin", binary);
    write_file(data() / "animals" / "cat.txt", "meow");
    write_file(data() / "animals" / "empty.txt", "");
    write_file(data() / "animals" / std::string(255, 'n'), "longest name");  // no room for ".part"

    LiveServer server{data()};
    const FetchStats stats = fetch_from(server.port(), "animals");
    expect_identical("animals");
    EXPECT_EQ(stats.files, 4u);
    EXPECT_EQ(stats.bytes, binary.size() + 4 + 12);
}

TEST_F(FetchTest, ManyFilesAreRequestedInBatches) {
    for (int i = 0; i < 40; ++i) {
        write_file(data() / "many" / std::format("f{:02}", i), std::format("file {}", i));
    }
    LiveServer server{data()};
    EXPECT_EQ(fetch_from(server.port(), "many").files, 40u);
    expect_identical("many");
}

TEST_F(FetchTest, RefetchReplacesFilesAndLeavesOthersAlone) {
    write_file(data() / "t" / "a.txt", "new");
    write_file(out() / "t" / "a.txt", "old contents");
    write_file(out() / "t" / "mine.txt", "not from the server");
    LiveServer server{data()};
    (void)fetch_from(server.port(), "t");
    EXPECT_EQ(read_file(out() / "t" / "a.txt"), "new");
    EXPECT_EQ(read_file(out() / "t" / "mine.txt"), "not from the server");
}

TEST_F(FetchTest, StaleTemporaryFileIsOverwritten) {
    write_file(data() / "t" / "a.txt", "short");
    write_file(out() / "t" / ".axon-0.part", std::string(100'000, 'x'));
    LiveServer server{data()};
    (void)fetch_from(server.port(), "t");
    expect_identical("t");
}

TEST_F(FetchTest, UnknownTopicIsAServerError) {
    write_file(data() / "t" / "a.txt", "a");
    LiveServer server{data()};
    try {
        (void)fetch_from(server.port(), "zebras");
        FAIL() << "fetch should have failed";
    } catch (const axon::ServerError& e) {
        EXPECT_EQ(e.code, ErrCode::UnknownTopic) << e.what();
    }
    EXPECT_FALSE(fs::exists(out() / "zebras"));
}

TEST_F(FetchTest, FileThatShrankOnTheServerLeavesNothingBehind) {
    write_file(data() / "t" / "a.bin", std::string(200'000, 'a'));
    LiveServer server{data()};
    fs::resize_file(data() / "t" / "a.bin", 10);
    try {
        (void)fetch_from(server.port(), "t");
        FAIL() << "fetch should have failed";
    } catch (const axon::ServerError& e) {
        EXPECT_EQ(e.code, ErrCode::Internal) << e.what();
    }
    EXPECT_FALSE(fs::exists(out() / "t" / "a.bin"));
}

TEST_F(FetchTest, ListsTheServersTopics) {
    write_file(data() / "plants" / "fern.txt", "green");
    write_file(data() / "animals" / "cat.txt", "meow");
    write_file(data() / "animals" / "dog.txt", "woof!");
    std::filesystem::create_directories(data() / "empty");
    LiveServer server{data()};
    const std::vector<TopicInfo> expected{{"animals", 2, 9}, {"empty", 0, 0}, {"plants", 1, 5}};
    EXPECT_EQ(axon::list_topics("127.0.0.1", server.port(), 5s), expected);
}

TEST_F(FetchTest, ListOfAnEmptyDatasetIsEmpty) {
    std::filesystem::create_directories(data());
    LiveServer server{data()};
    EXPECT_TRUE(axon::list_topics("127.0.0.1", server.port(), 5s).empty());
}

TEST_F(FetchTest, ListGetsProtocolErrorForAWrongReply) {
    const FakeServer server{[](Socket& sock) {
        MessageReader reader;
        (void)reader.read(sock);
        send_message(sock, Ack{});
    }};
    EXPECT_THROW((void)axon::list_topics("127.0.0.1", server.port(), 5s), axon::ProtocolError);
}

// --- surviving lost connections ---

TEST_F(FetchTest, SurvivesFrequentCutsAndResumesAtTheExactByte) {
    std::string big(3'000'000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>(i * 7 % 251);
    }
    write_file(data() / "t" / "big.bin", big);
    std::uint64_t total = big.size();
    for (int i = 0; i < 20; ++i) {
        const std::string small(1000 + static_cast<std::size_t>(i) * 3000, static_cast<char>('a' + i));
        write_file(data() / "t" / std::format("small-{:02}", i), small);
        total += small.size();
    }
    LiveServer server{data(), {.idle_timeout = 5s, .fault = {.probability = 0.7, .seed = 3}}};
    std::size_t retries = 0;
    const FetchStats stats = axon::fetch(
        {.host = "127.0.0.1",
         .port = server.port(),
         .topic = "t",
         .output = out(),
         .timeout = 5s,
         // Only 3 attempts in a row may fail without progress, so surviving
         // more cuts than that shows progress resets the count.
         .retry = {.max_failures = 3, .first_delay = 1ms, .max_delay = 10ms},
         .on_retry = [&retries](const std::string&, std::chrono::milliseconds) { ++retries; }});
    expect_identical("t");
    EXPECT_EQ(stats.bytes, total);
    EXPECT_GT(stats.connections, 4u) << "p = 0.7 should have cut more than 3 connections";
    EXPECT_EQ(retries, stats.connections - 1);

    server.stop();
    const auto s = server.stats();
    EXPECT_EQ(s.sessions_finished, 1u);
    EXPECT_EQ(s.payload_bytes - s.redundant_bytes, total) << "every byte reached the client once";
    // The client keeps every byte that arrived, even from a frame cut short,
    // and asks again from exactly there: nothing is ever sent twice.
    EXPECT_EQ(s.redundant_bytes, 0u);
}

// A scripted file of `n` bytes, sent in DATA frames by the fake servers below.
std::string patterned(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>(i * 13 % 256);
    }
    return s;
}

void send_frames(Socket& sock, std::string_view content, std::size_t from, std::size_t frames) {
    for (std::size_t f = 0; f < frames && from < content.size(); ++f) {
        const std::size_t n = std::min(max_data_len, content.size() - from);
        send_message(sock, Data{.file_id = 0, .offset = from, .bytes = bytes(content.substr(from, n))});
        from += n;
    }
}

TEST_F(FetchTest, ResumesAtTheExactByteEvenMidFrame) {
    const std::string content = patterned(300'000);
    const FileEntry entry{"x.bin", content.size(), crc_of(content)};
    std::uint64_t resumed_at = 0;
    {
        const FakeServer server{
            [&](Socket& sock, int i) {
                MessageReader reader;
                (void)reader.read(sock);  // GET
                send_message(sock, Ok{.session_id = 9, .files = std::vector<FileEntry>{entry}});
                const auto req = std::get<Req>(*reader.read(sock));
                if (i == 0) {
                    send_frames(sock, content, 0, 2);  // two whole frames...
                    const Data third{.file_id = 0,
                                     .offset = 2 * max_data_len,
                                     .bytes = bytes(std::string_view{content}.substr(2 * max_data_len,
                                                                                     max_data_len))};
                    send_truncated(sock, third, data_header_len + 1000);  // ...and part of a third
                    return;
                }
                resumed_at = req.ranges.at(0).offset;
                send_frames(sock, content, resumed_at, content.size());
                (void)reader.read(sock);  // ACK
            },
            2};
        (void)axon::fetch({.host = "127.0.0.1",
                           .port = server.port(),
                           .topic = "t",
                           .output = out(),
                           .timeout = 5s,
                           .retry = {.first_delay = 1ms, .max_delay = 1ms}});
    }
    EXPECT_EQ(resumed_at, 2 * max_data_len + 1000) << "the bytes of the cut frame are kept too";
    EXPECT_EQ(read_file(out() / "t" / "x.bin"), content);
}

TEST_F(FetchTest, ResumesAfterAConnectionReset) {
    const std::string content = patterned(200'000);
    const FileEntry entry{"x.bin", content.size(), crc_of(content)};
    {
        const FakeServer server{
            [&](Socket& sock, int i) {
                MessageReader reader;
                (void)reader.read(sock);  // GET
                send_message(sock, Ok{.session_id = 9, .files = std::vector<FileEntry>{entry}});
                const auto req = std::get<Req>(*reader.read(sock));
                if (i == 0) {
                    send_frames(sock, content, 0, 1);
                    const linger abort_on_close{.l_onoff = 1, .l_linger = 0};
                    ::setsockopt(sock.fd(), SOL_SOCKET, SO_LINGER, &abort_on_close,
                                 sizeof abort_on_close);
                    return;  // closing now sends a TCP reset: the client sees ECONNRESET
                }
                send_frames(sock, content, req.ranges.at(0).offset, content.size());
                (void)reader.read(sock);  // ACK
            },
            2};
        const FetchStats stats = axon::fetch({.host = "127.0.0.1",
                                              .port = server.port(),
                                              .topic = "t",
                                              .output = out(),
                                              .timeout = 5s,
                                              .retry = {.first_delay = 1ms, .max_delay = 1ms}});
        EXPECT_EQ(stats.connections, 2u);
    }
    EXPECT_EQ(read_file(out() / "t" / "x.bin"), content);
}

TEST_F(FetchTest, ForgottenSessionStartsOver) {
    static constexpr std::string_view content = "abcdef";
    const FileEntry entry{"x.txt", content.size(), crc_of(content)};
    std::vector<Get> gets(3);
    {
        const FakeServer server{
            [&](Socket& sock, int i) {
                MessageReader reader;
                gets[static_cast<std::size_t>(i)] = std::get<Get>(*reader.read(sock));
                if (i == 1) {  // the server lost the session in between
                    send_message(sock, Err{.code = ErrCode::SessionNotFound, .reason = "expired"});
                    return;
                }
                send_message(sock, Ok{.session_id = i == 0 ? 77u : 88u,
                                      .files = std::vector<FileEntry>{entry}});
                const auto req = std::get<Req>(*reader.read(sock));
                const auto bytes_from = [&](std::size_t n) { return bytes(content.substr(0, n)); };
                if (i == 0) {
                    send_message(sock, Data{.file_id = 0, .offset = 0, .bytes = bytes_from(3)});
                    return;  // hang up half-way
                }
                EXPECT_EQ(req.ranges.at(0).offset, 0u) << "a new session starts from scratch";
                send_message(sock, Data{.file_id = 0, .offset = 0, .bytes = bytes_from(6)});
                (void)reader.read(sock);  // ACK
            },
            3};
        (void)axon::fetch({.host = "127.0.0.1",
                           .port = server.port(),
                           .topic = "t",
                           .output = out(),
                           .timeout = 5s,
                           .retry = {.first_delay = 1ms, .max_delay = 1ms}});
    }
    EXPECT_EQ(read_file(out() / "t" / "x.txt"), content);
    EXPECT_EQ(gets[0].session_id, 0u);
    EXPECT_EQ(gets[1].session_id, 77u) << "the client tries to resume first";
    EXPECT_EQ(gets[2].session_id, 0u);
    EXPECT_TRUE(gets[2].want_list);
}

TEST_F(FetchTest, GivesUpAfterAttemptsThatReceiveNothing) {
    const FakeServer server{
        [](Socket& sock, int) {
            MessageReader reader;
            (void)reader.read(sock);
            send_message(sock, Ok{.session_id = 5, .files = std::vector<FileEntry>{{"x", 10, 0}}});
        },  // then hangs up before any DATA, every time
        3};
    std::size_t retries = 0;
    EXPECT_THROW((void)axon::fetch({.host = "127.0.0.1",
                                    .port = server.port(),
                                    .topic = "t",
                                    .output = out(),
                                    .timeout = 2s,
                                    .retry = {.max_failures = 3, .first_delay = 1ms, .max_delay = 1ms},
                                    .on_retry = [&retries](const std::string&,
                                                           std::chrono::milliseconds) { ++retries; }}),
                 axon::ConnectionClosed);
    EXPECT_EQ(retries, 2u) << "three attempts in all";
}

TEST_F(FetchTest, DiskErrorsAreNotRetried) {
    write_file(data() / "t" / "a.txt", "a");
    write_file(out(), "a file where the output folder should be");
    LiveServer server{data()};
    std::size_t retries = 0;
    EXPECT_THROW((void)axon::fetch({.host = "127.0.0.1",
                                    .port = server.port(),
                                    .topic = "t",
                                    .output = out(),
                                    .on_retry = [&retries](const std::string&,
                                                           std::chrono::milliseconds) { ++retries; }}),
                 fs::filesystem_error);
    EXPECT_EQ(retries, 0u);
}

// --- before or without a server ---

TEST_F(FetchTest, InvalidTopicIsRejectedBeforeConnecting) {
    // Port 1 has no server: getting invalid_argument proves nothing was dialled.
    EXPECT_THROW((void)fetch_from(1, "../etc"), std::invalid_argument);
    EXPECT_THROW((void)fetch_from(1, ""), std::invalid_argument);
}

TEST_F(FetchTest, RefusedFirstConnectionFailsAtOnce) {
    const std::uint16_t port = axon::Listener::listen("127.0.0.1", 0).local_port();  // now closed
    const auto t0 = std::chrono::steady_clock::now();
    try {
        (void)fetch_from(port, "t");
        FAIL() << "fetch should have failed";
    } catch (const axon::NetworkError& e) {
        EXPECT_EQ(e.code(), std::errc::connection_refused) << e.what();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 500ms) << "a wrong address is not retried";
}

// --- against servers that misbehave ---

// Answers GET with a one-file list and the first REQ with `data`, then waits
// for the client to hang up.
FakeServer serve_one_file(FileEntry entry, Data data) {
    return FakeServer{[entry, data](Socket& sock) {
        MessageReader reader;
        (void)reader.read(sock);
        send_message(sock, Ok{.files = std::vector<FileEntry>{entry}});
        (void)reader.read(sock);
        send_message(sock, data);
        while (reader.read(sock)) {
        }
    }};
}

TEST_F(FetchTest, CorruptFileIsRejectedAndNotKept) {
    static constexpr std::string_view content = "abc";
    const FakeServer server = serve_one_file({"x.txt", 3, crc_of(content) ^ 1},
                                             {.file_id = 0, .offset = 0, .bytes = bytes(content)});
    EXPECT_THROW((void)fetch_from(server.port(), "t"), axon::IntegrityError);
    EXPECT_TRUE(listing(out() / "t").empty()) << "neither x.txt nor its .part may remain";
}

TEST_F(FetchTest, DataAtTheWrongOffsetIsAProtocolError) {
    static constexpr std::string_view content = "abc";
    const FakeServer server = serve_one_file({"x.txt", 4, 0},
                                             {.file_id = 0, .offset = 1, .bytes = bytes(content)});
    EXPECT_THROW((void)fetch_from(server.port(), "t"), axon::ProtocolError);
    EXPECT_FALSE(fs::exists(out() / "t" / "x.txt"));
}

TEST_F(FetchTest, TooMuchDataIsAProtocolError) {
    static constexpr std::string_view content = "abcdef";
    const FakeServer server = serve_one_file({"x.txt", 3, crc_of("abc")},
                                             {.file_id = 0, .offset = 0, .bytes = bytes(content)});
    EXPECT_THROW((void)fetch_from(server.port(), "t"), axon::ProtocolError);
}

TEST_F(FetchTest, HangUpIsReportedOnceRetryingGivesUp) {
    const FakeServer server{[](Socket& sock) {
        MessageReader reader;
        (void)reader.read(sock);
        send_message(sock, Ok{.session_id = 1, .files = std::vector<FileEntry>{{"x.txt", 10, 0}}});
        (void)reader.read(sock);
    }};  // returns, closing the socket before any DATA
    EXPECT_THROW((void)axon::fetch({.host = "127.0.0.1",
                                    .port = server.port(),
                                    .topic = "t",
                                    .output = out(),
                                    .retry = {.max_failures = 1}}),
                 axon::ConnectionClosed);
    EXPECT_FALSE(fs::exists(out() / "t" / "x.txt"));
}

TEST_F(FetchTest, ServerErrorTextCannotInjectTerminalCodes) {
    const FakeServer server{[](Socket& sock) {
        MessageReader reader;
        (void)reader.read(sock);
        send_message(sock, Err{.code = ErrCode::Internal, .reason = "bad\x1b[2Jthing\n"});
    }};
    try {
        (void)fetch_from(server.port(), "t");
        FAIL() << "fetch should have failed";
    } catch (const axon::ServerError& e) {
        EXPECT_EQ(std::string_view{e.what()}, "server refused: INTERNAL: bad?[2Jthing?");
    }
}

}  // namespace
