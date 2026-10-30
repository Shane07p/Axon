#include "axon/log.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace axon {

void log_line(std::string_view line) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const std::string out = std::format("{:%F %T}Z {}\n", now, line);
    static std::mutex mu;
    const std::lock_guard lock{mu};
    std::fwrite(out.data(), 1, out.size(), stderr);
}

}  // namespace axon
