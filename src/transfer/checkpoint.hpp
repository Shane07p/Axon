#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "axon/protocol/messages.hpp"

namespace axon::detail {

struct Saved {
    proto::FileEntry entry{};
    std::uint64_t have = 0;
    std::uint32_t have_crc = 0;
    bool operator==(const Saved&) const = default;
};

// Missing, unreadable or malformed state means a fresh download.
std::vector<Saved> load_checkpoint(const std::filesystem::path& file);

// Flush the new state before replacing the old one, then flush the directory.
void save_checkpoint(const std::filesystem::path& file, std::span<const Saved> files);

}  // namespace axon::detail
