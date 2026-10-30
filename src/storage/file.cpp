#include "axon/storage/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include "sys_error.hpp"

namespace axon {

File::File(Fd fd, std::filesystem::path path) noexcept
    : fd_{std::move(fd)}, path_{std::move(path)} {}

File File::open(const std::filesystem::path& path, int flags) {
    // O_NONBLOCK stops a FIFO with no writer from blocking open(). It has no
    // effect on regular files, the only kind accepted below. O_NOFOLLOW
    // refuses a symlink as the last path component (ELOOP), so a link planted
    // in a dataset or output folder cannot redirect reads or writes.
    Fd fd{::open(path.c_str(), flags | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0644)};
    if (!fd) {
        detail::throw_errno("open", path.native());
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        detail::throw_errno("stat", path.native());
    }
    if (!S_ISREG(st.st_mode)) {
        throw std::system_error(S_ISDIR(st.st_mode) ? EISDIR : EINVAL, std::generic_category(),
                                "open " + path.native() + ": not a regular file");
    }
    return File{std::move(fd), path};
}

File File::open_read(const std::filesystem::path& path) { return open(path, O_RDONLY); }

File File::open_write(const std::filesystem::path& path) { return open(path, O_RDWR | O_CREAT); }

std::size_t File::read_at(std::span<std::byte> buf, std::uint64_t offset) const {
    std::size_t done = 0;
    while (done < buf.size()) {
        const ssize_t n = ::pread(fd_.get(), buf.data() + done, buf.size() - done,
                                  static_cast<off_t>(offset + done));
        if (n == 0) {
            break;  // end of file
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            detail::throw_errno("read", path_.native());
        }
        done += static_cast<std::size_t>(n);
    }
    return done;
}

void File::write_at(std::span<const std::byte> data, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::pwrite(fd_.get(), data.data() + done, data.size() - done,
                                   static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            detail::throw_errno("write", path_.native());
        }
        done += static_cast<std::size_t>(n);
    }
}

std::uint64_t File::size() const {
    struct stat st{};
    if (::fstat(fd_.get(), &st) != 0) {
        detail::throw_errno("stat", path_.native());
    }
    return static_cast<std::uint64_t>(st.st_size);
}

void File::truncate(std::uint64_t length) {
    if (::ftruncate(fd_.get(), static_cast<off_t>(length)) != 0) {
        detail::throw_errno("truncate", path_.native());
    }
}

}  // namespace axon
