#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axon/net/endpoint.hpp"

namespace {

using axon::Endpoint;
using axon::parse_endpoint;
using axon::parse_port;

TEST(Endpoint, HostAndPort) {
    EXPECT_EQ(parse_endpoint("localhost:9000", 1), (Endpoint{"localhost", 9000}));
    EXPECT_EQ(parse_endpoint("10.0.0.7:0", 1), (Endpoint{"10.0.0.7", 0}));
    EXPECT_EQ(parse_endpoint("[::1]:8080", 1), (Endpoint{"::1", 8080}));
}

TEST(Endpoint, MissingPortUsesDefault) {
    EXPECT_EQ(parse_endpoint("files.example", 9000), (Endpoint{"files.example", 9000}));
    EXPECT_EQ(parse_endpoint("[fe80::1]", 9000), (Endpoint{"fe80::1", 9000}));
}

TEST(Endpoint, RejectsMalformedInput) {
    const std::vector<std::string> bad{"",          ":9000",       "host:",  "host:65536",
                                       "host:-1",   "host:9x",     "::1",    "::1:9000",
                                       "[::1",      "[::1]9000",   "[]:9000"};
    for (const auto& s : bad) {
        EXPECT_THROW((void)parse_endpoint(s, 9000), std::invalid_argument) << "'" << s << "'";
    }
}

TEST(Port, Range) {
    EXPECT_EQ(parse_port("0"), 0);
    EXPECT_EQ(parse_port("65535"), 65535);
    EXPECT_THROW((void)parse_port("65536"), std::invalid_argument);
    EXPECT_THROW((void)parse_port(""), std::invalid_argument);
    EXPECT_THROW((void)parse_port(" 80"), std::invalid_argument);
    EXPECT_THROW((void)parse_port("+80"), std::invalid_argument);
}

}  // namespace
