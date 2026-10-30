#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "axon/fd.hpp"

namespace axon {

// An open regular file with positional reads and writes. Closes itself when
// destroyed; move-only. Errors throw std::system_error naming the path,
// including opening anything but a regular file (directory, FIFO, device).
class File {
public:
    File() noexcept = default;

    [[nodiscard]] static File open_read(const std::filesystem::path& path);

    // Opens for reading and writing, creating the file if missing. Existing
    // content is kept, so a partly downloaded file can be resumed.
    [[nodiscard]] static File open_write(const std::filesystem::path& path);

    // Reads into `buf` starting at `offset`. Returns the bytes read, which is
    // less than buf.size() only at end of file.
    [[nodiscard]] std::size_t read_at(std::span<std::byte> buf, std::uint64_t offset) const;

    // Writes all of `data` starting at `offset`.
    void write_at(std::span<const std::byte> data, std::uint64_t offset);

    [[nodiscard]] std::uint64_t size() const;
    void truncate(std::uint64_t length);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(fd_); }

private:
    File(Fd fd, std::filesystem::path path) noexcept;
    static File open(const std::filesystem::path& path, int flags);

    Fd fd_;
    std::filesystem::path path_;
};

}  // namespace axon
