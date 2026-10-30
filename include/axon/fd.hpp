#pragma once

#include <utility>

namespace axon {

// Owns one POSIX file descriptor and closes it when destroyed. Move-only.
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_{fd} {}
    ~Fd() { reset(); }

    Fd(Fd&& other) noexcept : fd_{other.release()} {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    // Gives up ownership without closing; the caller must close the result.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    // Closes the current descriptor (if any) and takes ownership of `fd`.
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

}  // namespace axon
