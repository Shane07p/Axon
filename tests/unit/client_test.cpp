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

// --- before or without a server ---

TEST_F(FetchTest, InvalidTopicIsRejectedBeforeConnecting) {
    // Port 1 has no server: getting invalid_argument proves nothing was dialled.
    EXPECT_THROW((void)fetch_from(1, "../etc"), std::invalid_argument);
    EXPECT_THROW((void)fetch_from(1, ""), std::invalid_argument);
}

TEST_F(FetchTest, RefusedConnectionIsASystemError) {
    const std::uint16_t port = axon::Listener::listen("127.0.0.1", 0).local_port();  // now closed
    try {
        (void)fetch_from(port, "t");
        FAIL() << "fetch should have failed";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), std::errc::connection_refused) << e.what();
    }
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

TEST_F(FetchTest, ServerThatHangsUpMidTransferIsReported) {
    const FakeServer server{[](Socket& sock) {
        MessageReader reader;
        (void)reader.read(sock);
        send_message(sock, Ok{.files = std::vector<FileEntry>{{"x.txt", 10, 0}}});
        (void)reader.read(sock);
    }};  // returns, closing the socket before any DATA
    EXPECT_THROW((void)fetch_from(server.port(), "t"), axon::ConnectionClosed);
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
