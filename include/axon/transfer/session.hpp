#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "axon/storage/dataset.hpp"

namespace axon {

struct SessionStats {
    std::uint64_t payload = 0;    // file bytes sent
    std::uint64_t redundant = 0;  // of those, bytes this session had been sent before
    std::uint32_t connections = 0;
};

// One client's download of one topic, which may span many connections.
// Thread-safe: several connections may use it at once.
class Session {
public:
    using Clock = std::chrono::steady_clock;

    Session(std::uint64_t id, const Topic& topic);

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const Topic& topic() const noexcept { return *topic_; }
    [[nodiscard]] Clock::time_point started() const noexcept { return started_; }

    void connected();

    // Records that bytes [offset, offset + length) of `file_id` were sent, and
    // how many of them had been sent before. Uses a per-file high-water mark,
    // which is exact while each file is requested front to back.
    void record_sent(std::uint32_t file_id, std::uint64_t offset, std::uint64_t length);

    [[nodiscard]] SessionStats stats() const;
    [[nodiscard]] Clock::time_point last_active() const;

    // File bytes of the topic not yet sent in this session.
    [[nodiscard]] std::uint64_t bytes_left() const;

private:
    const std::uint64_t id_;
    const Topic* topic_;
    std::uint64_t total_bytes_ = 0;  // set once in the constructor
    const Clock::time_point started_ = Clock::now();
    mutable std::mutex mu_;
    std::vector<std::uint64_t> max_sent_;  // per file, guarded by mu_
    SessionStats stats_;                   // guarded by mu_
    Clock::time_point last_active_ = started_;
};

// The live sessions of a server. Thread-safe.
class SessionTable {
public:
    // A session with no connection and no activity for `idle_ttl` is dropped.
    explicit SessionTable(std::chrono::milliseconds idle_ttl) : ttl_{idle_ttl} {}

    // A new session with a random, unguessable, non-zero id.
    [[nodiscard]] std::shared_ptr<Session> create(const Topic& topic);

    // nullptr if the id is unknown, finished or expired.
    [[nodiscard]] std::shared_ptr<Session> find(std::uint64_t id);

    void finish(std::uint64_t id);
    [[nodiscard]] std::size_t size() const;

private:
    void expire_idle();  // requires mu_

    const std::chrono::milliseconds ttl_;
    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
};

}  // namespace axon
