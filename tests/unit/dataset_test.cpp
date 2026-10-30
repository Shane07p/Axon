#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axon/crc32.hpp"
#include "axon/storage/dataset.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using axon::Dataset;
using axon::proto::FileEntry;
using axon::test::write_file;

class DatasetTest : public axon::test::TempDirTest {};

std::uint32_t crc_of(std::string_view s) {
    return axon::crc32_update(0, std::as_bytes(std::span{s.data(), s.size()}));
}

std::vector<std::string> names(const axon::Topic& t) {
    std::vector<std::string> out;
    for (const auto& f : t.files) {
        out.push_back(f.name);
    }
    return out;
}

bool has_warning(const Dataset& d, std::string_view needle) {
    return std::ranges::any_of(d.warnings(), [&](const std::string& w) {
        return w.find(needle) != std::string::npos;
    });
}

std::string all_warnings(const Dataset& d) {
    std::string out;
    for (const auto& w : d.warnings()) {
        out += w + "\n";
    }
    return out;
}

TEST_F(DatasetTest, ScansTopicsWithSortedFilesSizesAndCrcs) {
    write_file(dir_ / "plants" / "fern.txt", "green");
    write_file(dir_ / "animals" / "b.txt", "bb");
    write_file(dir_ / "animals" / "a.txt", "a");
    write_file(dir_ / "animals" / "empty.bin", "");

    const auto d = Dataset::scan(dir_);
    ASSERT_EQ(d.topics().size(), 2u);
    const auto& animals = d.topics()[0];
    EXPECT_EQ(animals.name, "animals");
    EXPECT_EQ(d.topics()[1].name, "plants");
    const std::vector<FileEntry> expected{
        {"a.txt", 1, crc_of("a")}, {"b.txt", 2, crc_of("bb")}, {"empty.bin", 0, 0}};
    EXPECT_EQ(animals.files, expected);
    EXPECT_EQ(animals.file_path(1), dir_ / "animals" / "b.txt");
    EXPECT_TRUE(d.warnings().empty()) << all_warnings(d);
}

TEST_F(DatasetTest, SortsByBytesNotLocale) {
    for (const char* name : {"b", "B", "a", "_"}) {
        write_file(dir_ / "t" / name, "x");
    }
    const auto d = Dataset::scan(dir_);
    ASSERT_EQ(d.topics().size(), 1u);
    EXPECT_EQ(names(d.topics()[0]), (std::vector<std::string>{"B", "_", "a", "b"}));
}

TEST_F(DatasetTest, CrcCoversFilesLargerThanOneRead) {
    std::string big(200'000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>(i * 7 % 256);
    }
    write_file(dir_ / "t" / "big.bin", big);
    const auto d = Dataset::scan(dir_);
    ASSERT_EQ(d.topics().size(), 1u);
    ASSERT_EQ(d.topics()[0].files.size(), 1u);
    EXPECT_EQ(d.topics()[0].files[0].size, big.size());
    EXPECT_EQ(d.topics()[0].files[0].crc, crc_of(big));
}

TEST_F(DatasetTest, SkipsWhatItShouldNotServeAndSaysWhy) {
    write_file(dir_ / "animals" / "cat.txt", "meow");
    write_file(dir_ / "outside.txt", "secret");
    fs::create_symlink(dir_ / "outside.txt", dir_ / "animals" / "link.txt");
    fs::create_directory_symlink(dir_ / "animals", dir_ / "alias");
    write_file(dir_ / "animals" / "nested" / "deep.txt", "x");
    fs::create_directories(dir_ / "bad topic");
    ASSERT_EQ(::mkfifo((dir_ / "animals" / "pipe").c_str(), 0600), 0);

    const auto d = Dataset::scan(dir_);
    ASSERT_EQ(d.topics().size(), 1u);
    EXPECT_EQ(names(d.topics()[0]), std::vector<std::string>{"cat.txt"});
    EXPECT_EQ(d.warnings().size(), 6u) << all_warnings(d);
    EXPECT_TRUE(has_warning(d, "animals/link.txt: symlinks are not followed"));
    EXPECT_TRUE(has_warning(d, "alias: symlinks are not followed"));
    EXPECT_TRUE(has_warning(d, "animals/nested: nested folders"));
    EXPECT_TRUE(has_warning(d, "animals/pipe: not a regular file"));
    EXPECT_TRUE(has_warning(d, "outside.txt: not a topic folder"));
    EXPECT_TRUE(has_warning(d, "'bad topic': name cannot be requested"));
}

TEST_F(DatasetTest, UnreadableFileIsSkippedNotFatal) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can read any file";
    }
    write_file(dir_ / "t" / "ok.txt", "fine");
    write_file(dir_ / "t" / "locked.txt", "no");
    fs::permissions(dir_ / "t" / "locked.txt", fs::perms::none);

    const auto d = Dataset::scan(dir_);
    ASSERT_EQ(d.topics().size(), 1u);
    EXPECT_EQ(names(d.topics()[0]), std::vector<std::string>{"ok.txt"});
    EXPECT_TRUE(has_warning(d, "t/locked.txt: open")) << all_warnings(d);
}

TEST_F(DatasetTest, UnlistableTopicIsSkipped) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can list any folder";
    }
    write_file(dir_ / "good" / "a.txt", "a");
    write_file(dir_ / "locked" / "b.txt", "b");
    fs::permissions(dir_ / "locked", fs::perms::none);

    const auto d = Dataset::scan(dir_);
    fs::permissions(dir_ / "locked", fs::perms::owner_all);  // so TearDown can delete it
    ASSERT_EQ(d.topics().size(), 1u);
    EXPECT_EQ(d.topics()[0].name, "good");
    EXPECT_TRUE(has_warning(d, "skipped topic locked: cannot list it")) << all_warnings(d);
}

TEST_F(DatasetTest, EmptyTopicHasNoFiles) {
    fs::create_directories(dir_ / "empty");
    const auto d = Dataset::scan(dir_);
    ASSERT_EQ(d.topics().size(), 1u);
    EXPECT_TRUE(d.topics()[0].files.empty());
}

TEST_F(DatasetTest, FindIsExactAndCaseSensitive) {
    fs::create_directories(dir_ / "animals");
    fs::create_directories(dir_ / "plants");
    const auto d = Dataset::scan(dir_);
    ASSERT_NE(d.find("animals"), nullptr);
    EXPECT_EQ(d.find("animals")->name, "animals");
    EXPECT_NE(d.find("plants"), nullptr);
    EXPECT_EQ(d.find("Animals"), nullptr);
    EXPECT_EQ(d.find("anim"), nullptr);
    EXPECT_EQ(d.find("zebras"), nullptr);
}

TEST_F(DatasetTest, UnusableRootThrows) {
    EXPECT_THROW((void)Dataset::scan(dir_ / "missing"), fs::filesystem_error);
    write_file(dir_ / "file", "x");
    EXPECT_THROW((void)Dataset::scan(dir_ / "file"), fs::filesystem_error);
}

}  // namespace
