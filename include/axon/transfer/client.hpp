#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "axon/protocol/messages.hpp"

namespace axon {

// The server answered with ERR.
class ServerError : public std::runtime_error {
public:
    ServerError(proto::ErrCode c, const std::string& reason);
    proto::ErrCode code;
};

// The server broke the protocol: an unexpected message, offset or size.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A downloaded file does not match the CRC-32 the server announced.
class IntegrityError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// How fetch() reconnects after a lost connection.
struct RetryPolicy {
    // Give up after this many attempts in a row that receive nothing.
    int max_failures = 8;
    // Wait before reconnecting: starts at first_delay, doubles after each
    // attempt without progress up to max_delay, with random jitter.
    std::chrono::milliseconds first_delay{50};
    std::chrono::milliseconds max_delay{2000};
};

struct FetchRequest {
    std::string host{};
    std::uint16_t port = 0;
    std::string topic{};
    std::filesystem::path output{};  // files land in output/topic/
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};  // per send or receive
    RetryPolicy retry{};
    // Called before each reconnect with the reason and the wait.
    std::function<void(const std::string& reason, std::chrono::milliseconds wait)> on_retry{};
};

struct FetchStats {
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    std::size_t connections = 0;
    std::chrono::duration<double> elapsed{};
};

// Downloads every file of a topic into output/topic/, creating it if needed.
// Each file is written to a temporary ".axon-<id>.part", checked against its
// CRC-32, then renamed into place: a file under its real name is always
// complete. Other files in the folder are left alone.
//
// A connection lost part-way is resumed: fetch() reconnects with backoff,
// rejoins its session and asks for every unfinished file from the exact byte
// it has. If the server no longer knows the session, it starts over. The very
// first connect is not retried, so a wrong address fails at once.
//
// Throws std::invalid_argument for an invalid topic name, ServerError,
// ProtocolError, IntegrityError, std::system_error for disk errors, and
// NetworkError / ConnectionClosed once retrying gives up.
FetchStats fetch(const FetchRequest& req);

// Asks the server which topics it has, in name order. Throws ServerError,
// ProtocolError, or std::system_error / ConnectionClosed.
std::vector<proto::TopicInfo> list_topics(
    const std::string& host, std::uint16_t port,
    std::chrono::milliseconds timeout = std::chrono::seconds{30});

}  // namespace axon
