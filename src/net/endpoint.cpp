#include "axon/net/endpoint.hpp"

#include <charconv>
#include <format>
#include <stdexcept>
#include <system_error>

namespace axon {

std::uint16_t parse_port(std::string_view s) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size() || value > 65535) {
        throw std::invalid_argument(std::format("invalid port '{}'", s));
    }
    return static_cast<std::uint16_t>(value);
}

Endpoint parse_endpoint(std::string_view s, std::uint16_t default_port) {
    std::string_view host = s;
    std::string_view port;
    bool has_port = false;
    if (s.starts_with('[')) {
        const auto close = s.find(']');
        if (close == std::string_view::npos) {
            throw std::invalid_argument(std::format("missing ']' in '{}'", s));
        }
        host = s.substr(1, close - 1);
        const auto rest = s.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') {
                throw std::invalid_argument(std::format("expected ':' after ']' in '{}'", s));
            }
            port = rest.substr(1);
            has_port = true;
        }
    } else if (const auto colon = s.rfind(':'); colon != std::string_view::npos) {
        if (s.find(':') != colon) {
            throw std::invalid_argument(
                std::format("put an IPv6 address in brackets, like [::1]:9000, not '{}'", s));
        }
        host = s.substr(0, colon);
        port = s.substr(colon + 1);
        has_port = true;
    }
    if (host.empty()) {
        throw std::invalid_argument(std::format("missing host in '{}'", s));
    }
    return {std::string{host}, has_port ? parse_port(port) : default_port};
}

}  // namespace axon
