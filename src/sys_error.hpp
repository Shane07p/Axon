#pragma once

#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>

namespace axon::detail {

// Throws E (a std::system_error) for the current errno, e.g.
// "open data/a.txt: No such file or directory". Takes views so nothing
// allocates (and possibly changes errno) before errno is read.
template <typename E = std::system_error>
[[noreturn]] void throw_errno(std::string_view op, std::string_view subject = {}) {
    const int err = errno;
    std::string what{op};
    if (!subject.empty()) {
        what += ' ';
        what += subject;
    }
    throw E(err, std::generic_category(), what);
}

}  // namespace axon::detail
