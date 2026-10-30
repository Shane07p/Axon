#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace axon {

// Writes `line` to stderr with a UTC timestamp, as one write. Thread-safe.
void log_line(std::string_view line);

template <typename... Args>
void log_msg(std::format_string<Args...> fmt, Args&&... args) {
    log_line(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace axon
