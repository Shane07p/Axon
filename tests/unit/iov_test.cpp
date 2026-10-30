#include <sys/uio.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "net/iov.hpp"

namespace {

using axon::detail::skip_written;

// Two iovecs over the strings `a` and `b`.
struct Parts {
    std::string a;
    std::string b;
    std::array<iovec, 2> iov{iovec{a.data(), a.size()}, iovec{b.data(), b.size()}};

    std::string_view left(std::size_t i) const {
        return {static_cast<const char*>(iov[i].iov_base), iov[i].iov_len};
    }
};

TEST(SkipWritten, PartOfFirst) {
    Parts p{"hello", "world"};
    EXPECT_EQ(skip_written(p.iov, 0, 3), 0u);
    EXPECT_EQ(p.left(0), "lo");
    EXPECT_EQ(p.left(1), "world");
}

TEST(SkipWritten, ExactlyFirst) {
    Parts p{"hello", "world"};
    EXPECT_EQ(skip_written(p.iov, 0, 5), 1u);
    EXPECT_EQ(p.left(1), "world");
}

TEST(SkipWritten, AcrossTheBoundary) {
    Parts p{"hello", "world"};
    EXPECT_EQ(skip_written(p.iov, 0, 7), 1u);
    EXPECT_EQ(p.left(1), "rld");
}

TEST(SkipWritten, ResumesFromLaterIndex) {
    Parts p{"hello", "world"};
    ASSERT_EQ(skip_written(p.iov, 0, 7), 1u);
    EXPECT_EQ(skip_written(p.iov, 1, 2), 1u);
    EXPECT_EQ(p.left(1), "d");
    EXPECT_EQ(skip_written(p.iov, 1, 1), 2u);
}

TEST(SkipWritten, Everything) {
    Parts p{"hello", "world"};
    EXPECT_EQ(skip_written(p.iov, 0, 10), 2u);
}

TEST(SkipWritten, SkipsEmptyParts) {
    Parts leading{"", "ab"};
    EXPECT_EQ(skip_written(leading.iov, 0, 0), 1u);
    Parts trailing{"ab", ""};
    EXPECT_EQ(skip_written(trailing.iov, 0, 2), 2u);
    Parts both{"", ""};
    EXPECT_EQ(skip_written(both.iov, 0, 0), 2u);
}

}  // namespace
