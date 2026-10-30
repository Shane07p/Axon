#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace axon {

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
    bool operator==(const Endpoint&) const = default;
};

// Parses "host:port", "host", "[ipv6]:port" or "[ipv6]"; a missing port is
// `default_port`. Throws std::invalid_argument.
[[nodiscard]] Endpoint parse_endpoint(std::string_view s, std::uint16_t default_port);

// Parses a decimal port, 0 to 65535. Throws std::invalid_argument.
[[nodiscard]] std::uint16_t parse_port(std::string_view s);

}  // namespace axon
