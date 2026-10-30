#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "axon/net/socket.hpp"
#include "axon/protocol/bytes.hpp"
#include "axon/protocol/frame.hpp"
#include "test_util.hpp"

namespace {

using namespace axon::proto;
using axon::ConnectionClosed;
using axon::DecodeError;
using axon::Socket;

class FrameTest : public axon::test::NoFdLeak {};

std::array<std::byte, frame_header_len> header(std::uint32_t type, std::uint32_t len) {
    std::array<std::byte, frame_header_len> h{};
    axon::store_be(std::span{h}.first<4>(), type);
    axon::store_be(std::span{h}.last<4>(), len);
    return h;
}

TEST_F(FrameTest, EveryMessageTypeArrivesIntact) {
    auto [a, b] = Socket::pair();
    const Get get{.topic = "animals", .session_id = 3, .want_list = true};
    const Ok ok{.session_id = 3, .dataset_id = 9, .files = std::vector<FileEntry>{{"x", 1, 2}}};
    const Err err{.code = ErrCode::BadRange, .reason = "past end"};
    const Req req{{{.file_id = 0, .offset = 0, .length = 1}}};
    const std::vector<std::byte> chunk(max_data_len, std::byte{0x5A});

    send_message(a, get);
    send_message(a, ok);
    send_message(a, err);
    send_message(a, req);
    send_message(a, Data{.file_id = 1, .offset = 64, .bytes = chunk});
    send_message(a, Ack{});
    a = Socket{};

    MessageReader reader;
    EXPECT_EQ(std::get<Get>(reader.read(b).value()), get);
    EXPECT_EQ(std::get<Ok>(reader.read(b).value()), ok);
    EXPECT_EQ(std::get<Err>(reader.read(b).value()), err);
    EXPECT_EQ(std::get<Req>(reader.read(b).value()), req);
    const auto data = std::get<Data>(reader.read(b).value());
    EXPECT_EQ(data.file_id, 1u);
    EXPECT_EQ(data.offset, 64u);
    EXPECT_TRUE(std::ranges::equal(data.bytes, chunk));
    EXPECT_TRUE(std::holds_alternative<Ack>(reader.read(b).value()));
    EXPECT_FALSE(reader.read(b).has_value());  // clean close after the last frame
}

TEST_F(FrameTest, WireFormatIsHeaderThenPayload) {
    auto [a, b] = Socket::pair();
    const Req req{{{.file_id = 7, .offset = 1, .length = 2}}};
    send_message(a, req);

    const auto payload = encode(req);
    std::vector<std::byte> wire(frame_header_len + payload.size());
    ASSERT_TRUE(b.recv_all(wire));
    const auto expected = header(tag("REQ "), static_cast<std::uint32_t>(payload.size()));
    EXPECT_TRUE(std::ranges::equal(std::span{wire}.first(frame_header_len), expected));
    EXPECT_TRUE(std::ranges::equal(std::span{wire}.subspan(frame_header_len), payload));
}

TEST_F(FrameTest, CloseInsideHeaderThrows) {
    auto [a, b] = Socket::pair();
    const auto h = header(tag("ACK "), 0);
    a.send_all(std::span{h}.first(3));
    a = Socket{};
    EXPECT_THROW((void)MessageReader{}.read(b), ConnectionClosed);
}

TEST_F(FrameTest, CloseRightAfterHeaderThrows) {
    auto [a, b] = Socket::pair();
    a.send_all(header(tag("REQ "), 22));
    a = Socket{};
    EXPECT_THROW((void)MessageReader{}.read(b), ConnectionClosed);
}

TEST_F(FrameTest, CloseInsidePayloadThrows) {
    auto [a, b] = Socket::pair();
    const std::array<std::byte, 4> part{};
    a.send_all(header(tag("REQ "), 22), part);
    a = Socket{};
    EXPECT_THROW((void)MessageReader{}.read(b), ConnectionClosed);
}

TEST_F(FrameTest, OversizedFrameIsRejectedBeforeItsPayload) {
    auto [a, b] = Socket::pair();
    a.send_all(header(tag("OK  "), 1000));
    // No payload follows: a reader that tried to read it would hit
    // ConnectionClosed instead of DecodeError.
    a = Socket{};
    MessageReader reader{16};
    EXPECT_THROW((void)reader.read(b), DecodeError);
}

TEST_F(FrameTest, MalformedPayloadThrowsDecodeError) {
    auto [a, b] = Socket::pair();
    const std::array<std::byte, 2> zero_ranges{};
    a.send_all(header(tag("REQ "), 2), zero_ranges);
    EXPECT_THROW((void)MessageReader{}.read(b), DecodeError);
}

TEST_F(FrameTest, MessageLargerThanSocketBufferArrivesIntact) {
    auto [a, b] = Socket::pair();
    Ok big{.session_id = 1, .files = std::vector<FileEntry>{}};
    for (std::uint32_t i = 0; i < 20000; ++i) {  // about 560 KB
        big.files->push_back({"file-" + std::to_string(i) + ".bin", i, i});
    }
    Socket& tx = a;
    std::jthread sender{[&tx, &big] { send_message(tx, big); }};
    MessageReader reader;
    EXPECT_EQ(std::get<Ok>(reader.read(b).value()), big);
}

}  // namespace
