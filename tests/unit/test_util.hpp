#pragma once

#include <cstddef>
#include <filesystem>
#include <iterator>

#include <gtest/gtest.h>

namespace axon::test {

inline std::size_t open_fd_count() {
    const std::filesystem::directory_iterator it{"/proc/self/fd"};
    return static_cast<std::size_t>(std::distance(begin(it), end(it)));
}

// Fails the test if it ends with a different number of open file descriptors
// than it started with.
class NoFdLeak : public ::testing::Test {
protected:
    void SetUp() override { fds_before_ = open_fd_count(); }
    void TearDown() override {
        EXPECT_EQ(open_fd_count(), fds_before_) << "file descriptor leak";
    }

private:
    std::size_t fds_before_ = 0;
};

}  // namespace axon::test
