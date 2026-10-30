#include "axon/transfer/session.hpp"

#include <sys/random.h>

#include <algorithm>
#include <cerrno>

#include "axon/log.hpp"
#include "sys_error.hpp"

namespace axon {
namespace {

std::uint64_t random_id() {
    std::uint64_t id = 0;
    while (::getrandom(&id, sizeof id, 0) != static_cast<ssize_t>(sizeof id)) {
        if (errno != EINTR) {
            detail::throw_errno("getrandom");
        }
    }
    return id;
}

}  // namespace

Session::Session(std::uint64_t id, const Topic& topic)
    : id_{id}, topic_{&topic}, max_sent_(topic.files.size(), 0) {}

void Session::connected() {
    const std::lock_guard lock{mu_};
    ++stats_.connections;
    last_active_ = Clock::now();
}

void Session::record_sent(std::uint32_t file_id, std::uint64_t offset, std::uint64_t length) {
    const std::lock_guard lock{mu_};
    std::uint64_t& high = max_sent_.at(file_id);
    const std::uint64_t end = offset + length;
    stats_.payload += length;
    stats_.redundant += high > offset ? std::min(high, end) - offset : 0;
    high = std::max(high, end);
    last_active_ = Clock::now();
}

SessionStats Session::stats() const {
    const std::lock_guard lock{mu_};
    return stats_;
}

Session::Clock::time_point Session::last_active() const {
    const std::lock_guard lock{mu_};
    return last_active_;
}

std::shared_ptr<Session> SessionTable::create(const Topic& topic) {
    const std::lock_guard lock{mu_};
    expire_idle();
    std::uint64_t id = 0;
    while (id == 0 || sessions_.contains(id)) {
        id = random_id();
    }
    auto session = std::make_shared<Session>(id, topic);
    sessions_.emplace(id, session);
    return session;
}

std::shared_ptr<Session> SessionTable::find(std::uint64_t id) {
    const std::lock_guard lock{mu_};
    expire_idle();
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionTable::finish(std::uint64_t id) {
    const std::lock_guard lock{mu_};
    sessions_.erase(id);
}

std::size_t SessionTable::size() const {
    const std::lock_guard lock{mu_};
    return sessions_.size();
}

void SessionTable::expire_idle() {
    const auto now = Session::Clock::now();
    const auto expired = std::erase_if(sessions_, [&](const auto& entry) {
        // use_count 1: only the table holds it, so no connection is using it.
        return entry.second.use_count() == 1 && now - entry.second->last_active() > ttl_;
    });
    if (expired > 0) {
        log_msg("expired {} idle session(s)", expired);
    }
}

}  // namespace axon
