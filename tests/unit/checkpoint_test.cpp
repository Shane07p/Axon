#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axon/crc32.hpp"
#include "axon/protocol/bytes.hpp"
#include "axon/storage/file.hpp"
#include "transfer/checkpoint.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using namespace axon::detail;
class CheckpointTest : public axon::test::TempDirTest {};

TEST_F(CheckpointTest, RoundTripsReplacesAndLeavesNoTemporaryFile) {
    const auto path = dir_ / ".axon-state";
    const std::vector<Saved> files{{{"a.txt", 1000, 42}, 600, 123}, {{"empty", 0, 0}, 0, 0}};
    save_checkpoint(path, files);
    EXPECT_EQ(load_checkpoint(path), files);
    save_checkpoint(path, std::span{files}.first(1));
    EXPECT_EQ(load_checkpoint(path), (std::vector<Saved>{files[0]}));
    EXPECT_FALSE(fs::exists(dir_ / ".axon-state.tmp"));
    save_checkpoint(path, {});
    EXPECT_TRUE(load_checkpoint(path).empty());
}

TEST_F(CheckpointTest, RejectsCorruptAndMalformedState) {
    const auto path = dir_ / ".axon-state";
    for (int kind = 0; kind < 10; ++kind) {
        SCOPED_TRACE(kind);
        axon::ByteWriter w;
        w.put_u32(kind == 0 ? 0 : axon::proto::tag("AXST"));
        w.put_u16(kind == 1 ? 2 : 1);
        w.put_u32(kind == 2 ? 2 : kind == 3 ? 1000 : 1);
        for (int n = 0; n < (kind == 2 ? 2 : 1); ++n) {
            w.put_str(kind == 4 ? "../x" : "x");
            w.put_u64(10);
            w.put_u32(42);
            w.put_u64(kind == 5 ? 11 : 5);
            w.put_u32(123);
        }
        if (kind == 6) {
            w.put_u8(0);  // valid CRC over a body with trailing data
        }
        auto buf = std::move(w).take();
        if (kind == 7) {
            buf[10] = std::byte{0xFF};  // string overruns the remaining record
        }
        w.put_bytes(buf);
        w.put_u32(axon::crc32_update(0, buf));
        buf = std::move(w).take();
        if (kind == 8) {
            buf.back() ^= std::byte{1};
        }
        if (kind == 9) {
            buf.resize(8);
        }
        auto file = axon::File::open_write(path);
        file.truncate(0);
        file.write_at(buf, 0);
        EXPECT_TRUE(load_checkpoint(path).empty());
    }
}

TEST_F(CheckpointTest, MissingOrNonregularStateIsIgnored) {
    EXPECT_TRUE(load_checkpoint(dir_ / "missing").empty());
    EXPECT_TRUE(load_checkpoint(dir_).empty());
    axon::test::write_file(dir_ / "target", "unrelated");
    fs::create_symlink(dir_ / "target", dir_ / ".axon-state");
    EXPECT_TRUE(load_checkpoint(dir_ / ".axon-state").empty());
    auto large = axon::File::open_write(dir_ / "large");
    large.truncate(64 * 1024 * 1024 + 1);
    EXPECT_TRUE(load_checkpoint(large.path()).empty());
}

TEST_F(CheckpointTest, FailedReplacementCleansUpItsTemporaryFile) {
    const auto path = dir_ / ".axon-state";
    fs::create_directory(path);
    const std::array<Saved, 1> files{{{{"x", 10, 42}, 5, 123}}};
    EXPECT_THROW(save_checkpoint(path, files), fs::filesystem_error);
    EXPECT_FALSE(fs::exists(dir_ / ".axon-state.tmp"));
    EXPECT_TRUE(fs::is_directory(path));
}

}  // namespace
