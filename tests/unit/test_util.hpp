#pragma once

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

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

// NoFdLeak plus a fresh, empty temporary directory `dir_` per test.
class TempDirTest : public NoFdLeak {
protected:
    void SetUp() override {
        NoFdLeak::SetUp();
        std::string tmpl = (std::filesystem::temp_directory_path() / "axon-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        dir_ = tmpl;
    }
    void TearDown() override {
        if (!dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
        }
        NoFdLeak::TearDown();
    }

    std::filesystem::path dir_;
};

inline void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary}.write(content.data(),
                                                static_cast<std::streamsize>(content.size()));
}

}  // namespace axon::test
