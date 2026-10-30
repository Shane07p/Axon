#include <sys/stat.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "axon/storage/file.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using axon::File;

// Each test gets its own empty temporary directory.
class FileTest : public axon::test::NoFdLeak {
protected:
    void SetUp() override {
        NoFdLeak::SetUp();
        std::string tmpl = (fs::temp_directory_path() / "axon-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        dir_ = tmpl;
    }
    void TearDown() override {
        if (!dir_.empty()) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
        NoFdLeak::TearDown();
    }

    fs::path dir_;
};

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span{s.data(), s.size()});
}

std::string read_all(const File& f) {
    std::string out(f.size(), '\0');
    out.resize(f.read_at(std::as_writable_bytes(std::span{out}), 0));
    return out;
}

TEST_F(FileTest, WriteAtThenReadBack) {
    const auto path = dir_ / "a.bin";
    {
        auto f = File::open_write(path);
        f.write_at(bytes("world"), 5);
        f.write_at(bytes("hello"), 0);
        EXPECT_EQ(f.size(), 10u);
    }
    EXPECT_EQ(read_all(File::open_read(path)), "helloworld");
}

TEST_F(FileTest, ReadAtStopsAtEndOfFile) {
    const auto path = dir_ / "a.bin";
    File::open_write(path).write_at(bytes("abc"), 0);
    const auto f = File::open_read(path);
    std::array<std::byte, 8> buf{};
    EXPECT_EQ(f.read_at(buf, 1), 2u);  // "bc", then end of file
    EXPECT_EQ(f.read_at(buf, 3), 0u);
    EXPECT_EQ(f.read_at(buf, 100), 0u);
}

TEST_F(FileTest, OpenWriteKeepsExistingContent) {
    const auto path = dir_ / "a.part";
    File::open_write(path).write_at(bytes("keep me"), 0);
    const auto f = File::open_write(path);
    EXPECT_EQ(read_all(f), "keep me");
}

TEST_F(FileTest, TruncateCutsTheTail) {
    auto f = File::open_write(dir_ / "a.part");
    f.write_at(bytes("0123456789"), 0);
    f.truncate(4);
    EXPECT_EQ(f.size(), 4u);
    EXPECT_EQ(read_all(f), "0123");
}

TEST_F(FileTest, RejectsDirectories) {
    for (const bool write : {false, true}) {
        try {
            (void)(write ? File::open_write(dir_) : File::open_read(dir_));
            FAIL() << "opened a directory, write=" << write;
        } catch (const std::system_error& e) {
            EXPECT_EQ(e.code(), std::errc::is_a_directory) << e.what();
        }
    }
}

TEST_F(FileTest, RejectsFifoWithoutBlocking) {
    const auto fifo = dir_ / "pipe";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    // No writer exists: a blocking open would hang here until the test times out.
    EXPECT_THROW((void)File::open_read(fifo), std::system_error);
    EXPECT_THROW((void)File::open_write(fifo), std::system_error);
}

TEST_F(FileTest, RejectsDevices) {
    try {
        (void)File::open_read("/dev/null");
        FAIL() << "opened a character device";
    } catch (const std::system_error& e) {
        EXPECT_NE(std::string_view{e.what()}.find("not a regular file"), std::string_view::npos)
            << e.what();
    }
}

TEST_F(FileTest, OpenMissingFileThrowsWithPath) {
    const auto path = dir_ / "missing.txt";
    try {
        (void)File::open_read(path);
        FAIL() << "open should have failed";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), std::errc::no_such_file_or_directory);
        EXPECT_NE(std::string_view{e.what()}.find(path.native()), std::string_view::npos)
            << e.what();
    }
}

}  // namespace
