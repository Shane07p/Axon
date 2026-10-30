#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

struct FetchRequest {
    std::string host{};
    std::uint16_t port = 0;
    std::string topic{};
    std::filesystem::path output{};  // files land in output/topic/
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};  // per send or receive
};

struct FetchStats {
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    std::chrono::duration<double> elapsed{};
};

// Downloads every file of a topic into output/topic/, creating it if needed.
// Each file is written to a temporary ".axon-<id>.part", checked against its
// CRC-32, then renamed into place: a file under its real name is always
// complete. Other files in the folder are left alone.
//
// Throws std::invalid_argument for an invalid topic name, ServerError,
// ProtocolError, IntegrityError, or std::system_error / ConnectionClosed for
// network and disk errors.
FetchStats fetch(const FetchRequest& req);

// Asks the server which topics it has, in name order. Throws ServerError,
// ProtocolError, or std::system_error / ConnectionClosed.
std::vector<proto::TopicInfo> list_topics(
    const std::string& host, std::uint16_t port,
    std::chrono::milliseconds timeout = std::chrono::seconds{30});

}  // namespace axon
