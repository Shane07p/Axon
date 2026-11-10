#include "transfer/checkpoint.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <unordered_set>

#include "axon/crc32.hpp"
#include "axon/protocol/bytes.hpp"
#include "axon/storage/file.hpp"
#include "sys_error.hpp"

namespace axon::detail {
namespace fs = std::filesystem;

std::vector<Saved> load_checkpoint(const fs::path& file) {
    try {
        const File in = File::open_read(file);
        const auto size = in.size();
        // Bound allocation before reading an untrusted local file.
        if (size < 14 || size > 64 * 1024 * 1024) {
            return {};
        }
        std::vector<std::byte> buf(static_cast<std::size_t>(size));
        if (in.read_at(buf, 0) != buf.size()) {
            return {};
        }
        const auto body = std::span{buf}.first(buf.size() - 4);
        if (crc32_update(0, body) != load_be<std::uint32_t>(std::span{buf}.last<4>())) {
            return {};
        }
        ByteReader r{body};
        if (r.get_u32() != proto::tag("AXST") || r.get_u16() != 1) {
            return {};
        }
        const auto count = r.get_u32();
        if (count > r.remaining() / 27) {  // shortest record: a one-byte name
            return {};
        }
        std::vector<Saved> saved;
        saved.reserve(count);
        std::unordered_set<std::string_view> seen;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto name = r.get_str();
            if (!proto::is_valid_filename(name) || !seen.insert(name).second) {
                return {};
            }
            Saved& s = saved.emplace_back();
            s.entry = {std::string{name}, r.get_u64(), r.get_u32()};
            s.have = r.get_u64();
            s.have_crc = r.get_u32();
            if (s.have > s.entry.size) {
                return {};
            }
        }
        r.expect_end();
        return saved;
    } catch (const DecodeError&) {
        return {};
    } catch (const std::system_error&) {
        return {};
    }
}

void save_checkpoint(const fs::path& file, std::span<const Saved> files) {
    ByteWriter w;
    w.put_u32(proto::tag("AXST"));
    w.put_u16(1);
    w.put_u32(static_cast<std::uint32_t>(files.size()));
    for (const Saved& s : files) {
        w.put_str(s.entry.name);
        w.put_u64(s.entry.size);
        w.put_u32(s.entry.crc);
        w.put_u64(s.have);
        w.put_u32(s.have_crc);
    }
    w.put_u32(crc32_update(0, w.view()));
    const fs::path tmp = file.string() + ".tmp";
    try {
        File out = File::open_write(tmp);
        out.truncate(0);
        out.write_at(w.view(), 0);
        out.sync();
        fs::rename(tmp, file);
        const fs::path parent = file.has_parent_path() ? file.parent_path() : fs::path{"."};
        const Fd dir{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (!dir) {
            throw_errno("open", parent.native());
        }
        while (::fsync(dir.get()) != 0) {
            if (errno != EINTR) {
                throw_errno("fsync", parent.native());
            }
        }
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

}  // namespace axon::detail
