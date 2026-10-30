#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axon/protocol/bytes.hpp"
#include "axon/protocol/messages.hpp"

namespace {

using namespace axon::proto;
using axon::ByteWriter;
using axon::DecodeError;

static_assert(static_cast<std::uint32_t>(MsgType::Data) == 0x44415441);  // "DATA"

std::uint32_t tag_of(const Message& m) { return static_cast<std::uint32_t>(type_of(m)); }

template <typename T>
T round_trip(const T& m) {
    const auto payload = encode(m);
    return std::get<T>(decode(tag_of(m), payload));
}

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

void expect_rejected(const Message& m) {
    EXPECT_THROW((void)decode(tag_of(m), encode(m)), DecodeError);
}

const std::vector<FileEntry> two_files{{"a.txt", 10, 0xAAAAAAAA}, {"b.bin", 0, 0}};

// --- round trips and layout ---

TEST(Messages, GetRoundTrips) {
    const Get m{.topic = "animals", .session_id = 42, .connections = 8, .want_list = true};
    EXPECT_EQ(round_trip(m), m);
    EXPECT_EQ(round_trip(Get{.topic = "t"}), Get{.topic = "t"});
}

TEST(Messages, GetNeedsAtLeastOneConnection) {
    expect_rejected(Get{.topic = "t", .connections = 0});
}

TEST(Messages, OkRoundTripsWithAndWithoutList) {
    const Ok with{.session_id = 7, .dataset_id = 0xFEEDFACECAFEBEEF, .files = two_files};
    EXPECT_EQ(round_trip(with), with);
    const Ok empty_list{.session_id = 7, .files = std::vector<FileEntry>{}};
    EXPECT_EQ(round_trip(empty_list), empty_list);
    const Ok without{.session_id = 7};
    EXPECT_EQ(round_trip(without), without);
}

TEST(Messages, ErrRoundTripsAndKeepsUnknownCodes) {
    const Err m{.code = ErrCode::UnknownTopic, .reason = "no topic 'birds'"};
    EXPECT_EQ(round_trip(m), m);
    const Err future{.code = static_cast<ErrCode>(999), .reason = ""};
    EXPECT_EQ(round_trip(future), future);
    EXPECT_EQ(to_string(future.code), "UNKNOWN_ERROR");
    EXPECT_EQ(to_string(ErrCode::BadRange), "BAD_RANGE");
}

TEST(Messages, ReqRoundTripsUpToMaxRanges) {
    Req m;
    for (std::uint32_t i = 0; i < max_req_ranges; ++i) {
        m.ranges.push_back({.file_id = i, .offset = i * 100ULL, .length = 50});
    }
    EXPECT_EQ(round_trip(m), m);
}

TEST(Messages, DataViewsThePayloadWithoutCopying) {
    const auto bytes = bytes_of({1, 2, 3});
    const auto payload = encode(Data{.file_id = 3, .offset = 1000, .bytes = bytes});
    const auto d = std::get<Data>(decode(tag_of(Data{}), payload));
    EXPECT_EQ(d.file_id, 3u);
    EXPECT_EQ(d.offset, 1000u);
    EXPECT_TRUE(std::ranges::equal(d.bytes, bytes));
    EXPECT_EQ(d.bytes.data(), payload.data() + 12);  // points into the payload
}

TEST(Messages, ListAndTopicsRoundTrip) {
    EXPECT_EQ(round_trip(List{}), List{});
    EXPECT_EQ(encode(List{}), bytes_of({0x00, 0x01}));  // just the version
    const Topics m{{{"animals", 3, 0xFFFF'FFFF'FFFF}, {"plants", 0, 0}}};
    EXPECT_EQ(round_trip(m), m);
    EXPECT_EQ(round_trip(Topics{}), Topics{});
}

TEST(Messages, ListFromAnotherVersionDecodesToJustTheVersion) {
    const auto payload = bytes_of({0x00, 0x05, 0xAB, 0xCD});  // v5, unknown layout
    EXPECT_EQ(std::get<List>(decode(tag_of(List{}), payload)).version, 5);
}

TEST(Messages, TopicsRejectsBadOrDuplicateNamesAndHugeCounts) {
    for (const std::string name : {"", "..", "a/b", "has space"}) {
        SCOPED_TRACE(name);
        expect_rejected(Topics{{{name, 1, 1}}});
    }
    expect_rejected(Topics{{{"same", 1, 1}, {"same", 2, 2}}});
    ByteWriter w;
    w.put_u32(std::numeric_limits<std::uint32_t>::max());  // 4 billion topics, 0 bytes of them
    EXPECT_THROW((void)decode(tag_of(Topics{}), w.view()), DecodeError);
}

TEST(Messages, AckHasEmptyPayload) {
    EXPECT_TRUE(encode(Ack{}).empty());
    EXPECT_TRUE(std::holds_alternative<Ack>(decode(tag_of(Ack{}), {})));
}

TEST(Messages, GetWireLayout) {
    const auto payload = encode(
        Get{.topic = "ab", .session_id = 0x0102030405060708, .connections = 4, .want_list = true});
    EXPECT_EQ(payload, bytes_of({0x00, 0x01,                                      // version
                                 0x00, 0x02, 'a', 'b',                            // topic
                                 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // session
                                 0x00, 0x04,                                      // connections
                                 0x01}));                                         // want_list
}

TEST(Messages, ReqWireLayout) {
    const auto payload = encode(Req{{{.file_id = 5, .offset = 0x10, .length = 0x20}}});
    EXPECT_EQ(payload, bytes_of({0x00, 0x01,                                // count
                                 0x00, 0x00, 0x00, 0x05,                    // file_id
                                 0, 0, 0, 0, 0, 0, 0, 0x10,                 // offset
                                 0, 0, 0, 0, 0, 0, 0, 0x20}));              // length
}

// --- framing-level damage ---

TEST(Messages, UnknownTypeIsRejected) {
    EXPECT_THROW((void)decode(tag("NOPE"), {}), DecodeError);
}

TEST(Messages, TruncatedOrPaddedPayloadsAreRejected) {
    const std::vector<Message> samples{
        Get{.topic = "animals", .session_id = 9},
        Ok{.session_id = 1, .dataset_id = 2},
        Ok{.session_id = 1, .dataset_id = 2, .files = two_files},
        Err{.code = ErrCode::Internal, .reason = "disk"},
        Req{{{.file_id = 1, .offset = 0, .length = 10}, {.file_id = 2, .offset = 5, .length = 1}}},
        Ack{},
        List{},
        Topics{},
        Topics{{{"animals", 3, 1'000'000}, {"plants", 1, 6}}},
    };
    for (const auto& m : samples) {
        const auto payload = encode(m);
        SCOPED_TRACE(testing::Message() << "type " << tag_of(m) << ", " << payload.size() << " bytes");
        for (std::size_t len = 0; len < payload.size(); ++len) {
            EXPECT_THROW((void)decode(tag_of(m), std::span{payload}.first(len)), DecodeError)
                << "prefix of " << len << " bytes was accepted";
        }
        auto padded = payload;
        padded.push_back(std::byte{0});
        EXPECT_THROW((void)decode(tag_of(m), padded), DecodeError) << "trailing byte was accepted";
    }
}

TEST(Messages, DataWithTruncatedHeaderIsRejected) {
    const auto payload = encode(Data{.file_id = 1, .offset = 2, .bytes = bytes_of({9})});
    for (std::size_t len = 0; len < payload.size(); ++len) {
        EXPECT_THROW((void)decode(tag_of(Data{}), std::span{payload}.first(len)), DecodeError) << len;
    }
}

// --- malformed fields ---

TEST(Messages, GetFromAnotherVersionDecodesToJustTheVersion) {
    const auto payload = bytes_of({0x00, 0x02, 0xFF, 0xFF, 0xFF});  // v2, unknown layout
    const auto m = std::get<Get>(decode(tag_of(Get{}), payload));
    EXPECT_EQ(m.version, 2);
    EXPECT_TRUE(m.topic.empty());
}

TEST(Messages, GetRejectsBadTopicsAndFlags) {
    const std::vector<std::string> bad{"",       ".",      "..",      "a/b", "a b",
                                       "tab\t", "nl\n", "del\x7F", std::string(max_topic_len + 1, 't')};
    for (const auto& topic : bad) {
        SCOPED_TRACE(topic);
        expect_rejected(Get{.topic = topic});
    }
    ByteWriter w;
    w.put_u16(protocol_version);
    w.put_str("ok");
    w.put_u64(0);
    w.put_u16(1);
    w.put_u8(2);  // want_list must be 0 or 1
    EXPECT_THROW((void)decode(tag_of(Get{}), w.view()), DecodeError);
}

TEST(Messages, OkRejectsUnsafeOrDuplicateFileNames) {
    const std::vector<std::string> bad{"",         ".",
                                       "..",       "../etc/passwd",
                                       "dir/file", std::string{"nul\0byte", 8},
                                       std::string(max_name_len + 1, 'n')};
    for (const auto& name : bad) {
        SCOPED_TRACE(name);
        expect_rejected(Ok{.files = std::vector<FileEntry>{{name, 1, 0}}});
    }
    expect_rejected(Ok{.files = std::vector<FileEntry>{{"same", 1, 0}, {"same", 2, 0}}});
}

TEST(Messages, OkRejectsCountLargerThanPayload) {
    ByteWriter w;
    w.put_u64(1);
    w.put_u64(2);
    w.put_u8(1);
    w.put_u32(std::numeric_limits<std::uint32_t>::max());  // 4 billion entries, 0 bytes of them
    EXPECT_THROW((void)decode(tag_of(Ok{}), w.view()), DecodeError);
}

TEST(Messages, OkRejectsBadListFlag) {
    ByteWriter w;
    w.put_u64(1);
    w.put_u64(2);
    w.put_u8(7);
    EXPECT_THROW((void)decode(tag_of(Ok{}), w.view()), DecodeError);
}

TEST(Messages, ReqRejectsBadRanges) {
    expect_rejected(Req{});  // no ranges
    expect_rejected(Req{std::vector<Range>(max_req_ranges + 1, {.file_id = 0, .offset = 0, .length = 1})});
    expect_rejected(Req{{{.file_id = 0, .offset = 0, .length = 0}}});
    expect_rejected(Req{{{.file_id = 0, .offset = std::numeric_limits<std::uint64_t>::max(), .length = 1}}});
}

TEST(Messages, DataRejectsBadSizes) {
    const std::vector<std::byte> too_big(max_data_len + 1);
    const auto one = bytes_of({1});
    expect_rejected(Data{.file_id = 0, .offset = 0, .bytes = {}});
    expect_rejected(Data{.file_id = 0, .offset = 0, .bytes = too_big});
    expect_rejected(Data{.file_id = 0, .offset = std::numeric_limits<std::uint64_t>::max(), .bytes = one});

    const std::vector<std::byte> largest(max_data_len);
    const auto payload = encode(Data{.file_id = 0, .offset = 0, .bytes = largest});
    EXPECT_EQ(std::get<Data>(decode(tag_of(Data{}), payload)).bytes.size(), max_data_len);
}

// --- name rules ---

TEST(Names, TopicRules) {
    EXPECT_TRUE(is_valid_topic("animals"));
    EXPECT_TRUE(is_valid_topic("data-2026_v1.0"));
    EXPECT_TRUE(is_valid_topic("caf\xC3\xA9"));  // UTF-8 is fine
    EXPECT_TRUE(is_valid_topic(std::string(max_topic_len, 't')));
    EXPECT_TRUE(is_valid_topic("..."));  // only "." and ".." are special
}

TEST(Names, FilenameRules) {
    EXPECT_TRUE(is_valid_filename("photo 01.jpg"));  // spaces are fine in file names
    EXPECT_TRUE(is_valid_filename("...hidden"));
    EXPECT_TRUE(is_valid_filename(".bashrc"));
    EXPECT_FALSE(is_valid_filename("a/b"));
    EXPECT_FALSE(is_valid_filename("."));
}

}  // namespace
