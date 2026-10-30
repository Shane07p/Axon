#include "axon/fd.hpp"

#include <unistd.h>

namespace axon {

void Fd::reset(int fd) noexcept {
    const int old = std::exchange(fd_, fd);
    if (old >= 0 && old != fd) {
        ::close(old);  // Linux frees the descriptor even if close() fails, so never retry.
    }
}

}  // namespace axon
