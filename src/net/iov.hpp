#pragma once

#include <sys/uio.h>

#include <algorithm>
#include <cstddef>
#include <span>

namespace axon::detail {

// After `n` bytes from iov[first..] were written, skips past them (and any
// empty iovecs): advances the partly written one and returns the index of the
// first iovec with bytes left, or iov.size() when everything is written.
inline std::size_t skip_written(std::span<iovec> iov, std::size_t first, std::size_t n) noexcept {
    while (first < iov.size() && (n > 0 || iov[first].iov_len == 0)) {
        iovec& v = iov[first];
        const std::size_t k = std::min(n, v.iov_len);
        v.iov_base = static_cast<std::byte*>(v.iov_base) + k;
        v.iov_len -= k;
        n -= k;
        if (v.iov_len == 0) {
            ++first;
        }
    }
    return first;
}

}  // namespace axon::detail
