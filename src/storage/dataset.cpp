#include "axon/storage/dataset.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "axon/crc32.hpp"
#include "axon/storage/file.hpp"

namespace axon {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t read_chunk = 64 * 1024;

// Reads the file once for its size and CRC-32. `buf` is reused across files.
proto::FileEntry measure(const fs::path& path, std::string name, std::vector<std::byte>& buf) {
    const File f = File::open_read(path);
    proto::FileEntry e{.name = std::move(name)};
    for (;;) {
        const std::size_t n = f.read_at(buf, e.size);
        e.crc = crc32_update(e.crc, std::span{buf}.first(n));
        e.size += n;
        if (n < buf.size()) {
            return e;  // end of file
        }
    }
}

// nullopt (with a warning) if the folder cannot be listed completely.
std::optional<Topic> scan_topic(const fs::path& dir, std::string name,
                                std::vector<std::string>& warnings, std::vector<std::byte>& buf) {
    Topic t{.name = std::move(name), .dir = dir};
    std::error_code list_ec;
    for (fs::directory_iterator it{dir, list_ec}, end; !list_ec && it != end;
         it.increment(list_ec)) {
        const std::string file = it->path().filename().string();
        const std::string where = "skipped " + t.name + "/" + file + ": ";
        std::error_code ec;
        const fs::file_status st = it->symlink_status(ec);
        if (fs::is_symlink(st)) {
            warnings.push_back(where + "symlinks are not followed");
        } else if (fs::is_directory(st)) {
            warnings.push_back(where + "nested folders are not transferred");
        } else if (!fs::is_regular_file(st)) {
            warnings.push_back(where + "not a regular file");
        } else if (!proto::is_valid_filename(file)) {
            warnings.push_back(where + "name cannot be sent");
        } else {
            try {
                t.files.push_back(measure(it->path(), file, buf));
            } catch (const std::system_error& e) {
                warnings.push_back(where + e.what());
            }
        }
    }
    if (list_ec) {
        warnings.push_back("skipped topic " + t.name + ": cannot list it: " + list_ec.message());
        return std::nullopt;
    }
    std::ranges::sort(t.files, {}, &proto::FileEntry::name);
    return t;
}

}  // namespace

Dataset Dataset::scan(const fs::path& root) {
    Dataset d;
    std::vector<std::byte> buf(read_chunk);
    for (const fs::directory_entry& entry : fs::directory_iterator{root}) {
        const std::string name = entry.path().filename().string();
        std::error_code ec;
        const fs::file_status st = entry.symlink_status(ec);
        if (fs::is_symlink(st)) {
            d.warnings_.push_back("skipped " + name + ": symlinks are not followed");
        } else if (!fs::is_directory(st)) {
            d.warnings_.push_back("skipped " + name + ": not a topic folder");
        } else if (!proto::is_valid_topic(name)) {
            d.warnings_.push_back("skipped topic '" + name + "': name cannot be requested");
        } else if (auto topic = scan_topic(entry.path(), name, d.warnings_, buf)) {
            d.topics_.push_back(std::move(*topic));
        }
    }
    std::ranges::sort(d.topics_, {}, &Topic::name);
    return d;
}

const Topic* Dataset::find(std::string_view name) const noexcept {
    const auto by_name = [](const Topic& t) -> std::string_view { return t.name; };
    const auto it = std::ranges::lower_bound(topics_, name, {}, by_name);
    return it != topics_.end() && it->name == name ? &*it : nullptr;
}

}  // namespace axon
