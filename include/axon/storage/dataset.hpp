#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "axon/protocol/messages.hpp"

namespace axon {

struct Topic {
    std::string name{};
    std::filesystem::path dir{};
    std::vector<proto::FileEntry> files{};  // sorted by name; a file's id is its index

    [[nodiscard]] std::filesystem::path file_path(std::size_t id) const {
        return dir / files.at(id).name;
    }
};

// The topics under a root folder, scanned once: every sub-folder is a topic
// and every regular file directly inside it is one of its files, with its size
// and CRC-32. Symlinks, nested folders, unreadable files and names the
// protocol cannot carry are skipped and reported in warnings().
class Dataset {
public:
    // Throws std::filesystem::filesystem_error if `root` cannot be listed.
    [[nodiscard]] static Dataset scan(const std::filesystem::path& root);

    // Exact, case-sensitive lookup; nullptr if there is no such topic.
    [[nodiscard]] const Topic* find(std::string_view name) const noexcept;

    [[nodiscard]] const std::vector<Topic>& topics() const noexcept { return topics_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }

private:
    std::vector<Topic> topics_;  // sorted by name
    std::vector<std::string> warnings_;
};

}  // namespace axon
