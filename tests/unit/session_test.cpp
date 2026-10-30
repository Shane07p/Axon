#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>

#include <gtest/gtest.h>

#include "axon/storage/dataset.hpp"
#include "axon/transfer/session.hpp"

namespace {

using namespace std::chrono_literals;
using axon::Session;
using axon::SessionTable;
using axon::Topic;

const Topic topic{.name = "t", .dir = "/nowhere", .files = {{"a", 100, 0}, {"b", 50, 0}}};

TEST(Session, CountsBytesSentAgainBelowTheHighWaterMark) {
    Session s{1, topic};
    s.record_sent(0, 0, 40);   // first 40 bytes
    s.record_sent(0, 40, 30);  // next 30: nothing repeated
    EXPECT_EQ(s.stats().redundant, 0u);
    s.record_sent(0, 50, 50);  // resumed at 50 after a cut: 50..70 again
    EXPECT_EQ(s.stats().payload, 120u);
    EXPECT_EQ(s.stats().redundant, 20u);
    s.record_sent(0, 0, 10);  // entirely old
    EXPECT_EQ(s.stats().redundant, 30u);
    s.record_sent(1, 0, 50);  // another file has its own mark
    EXPECT_EQ(s.stats().redundant, 30u);
    EXPECT_EQ(s.stats().payload, 180u);
}

TEST(Session, CountsConnections) {
    Session s{1, topic};
    s.connected();
    s.connected();
    EXPECT_EQ(s.stats().connections, 2u);
}

TEST(SessionTable, IdsAreRandomNonZeroAndUnique) {
    SessionTable table{10min};
    std::set<std::uint64_t> ids;
    for (int i = 0; i < 200; ++i) {
        const auto s = table.create(topic);
        EXPECT_NE(s->id(), 0u);
        ids.insert(s->id());
    }
    EXPECT_EQ(ids.size(), 200u);
    EXPECT_EQ(table.size(), 200u);
}

TEST(SessionTable, FindAndFinish) {
    SessionTable table{10min};
    const auto id = table.create(topic)->id();
    ASSERT_NE(table.find(id), nullptr);
    EXPECT_EQ(&table.find(id)->topic(), &topic);
    EXPECT_EQ(table.find(id + 1), nullptr);
    table.finish(id);
    EXPECT_EQ(table.find(id), nullptr);
}

TEST(SessionTable, IdleSessionExpires) {
    SessionTable table{50ms};
    const auto id = table.create(topic)->id();  // nobody keeps a reference
    std::this_thread::sleep_for(120ms);
    EXPECT_EQ(table.find(id), nullptr);
    EXPECT_EQ(table.size(), 0u);
}

TEST(SessionTable, SessionInUseDoesNotExpire) {
    SessionTable table{50ms};
    const std::shared_ptr<Session> held = table.create(topic);  // a connection is using it
    std::this_thread::sleep_for(120ms);
    EXPECT_EQ(table.find(held->id()), held);
}

TEST(SessionTable, ActivityKeepsASessionAlive) {
    SessionTable table{150ms};
    const auto id = table.create(topic)->id();
    for (int i = 0; i < 4; ++i) {
        std::this_thread::sleep_for(60ms);
        table.find(id)->record_sent(0, 0, 1);  // a connection sending now and then
    }
    EXPECT_NE(table.find(id), nullptr);
}

}  // namespace
