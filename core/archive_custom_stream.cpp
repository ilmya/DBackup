#include "core/archive_custom_stream.h"
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace dbackup::archive_custom_stream {
namespace {
constexpr uint64_t maxMetadata = 1024u * 1024u;
uint64_t get(const uint8_t* p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n |= uint64_t(p[i]) << (8 * i);
    return n;
}
void put(std::vector<uint8_t>& out, uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
}
bool readAt(int fd, uint64_t pos, uint8_t* data, size_t size) {
    while (size) {
        const ssize_t n = pread(fd, data, size, static_cast<off_t>(pos));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        pos += static_cast<size_t>(n);
        size -= static_cast<size_t>(n);
    }
    return true;
}
} // namespace

bool entryHeader(const archive_format::Entry& entry, uint64_t size, std::vector<uint8_t>& output,
                 std::string& error) {
    if (entry.file.type != 0 && size != 0) {
        error = "特殊文件不能包含普通文件数据";
        return false;
    }
    archive_format::Entry metadata = entry;
    metadata.data.clear();
    std::vector<uint8_t> encoded;
    if (!archive_format::encode({metadata}, 0, "", encoded, error)) return false;
    if (encoded.empty() || encoded.size() > maxMetadata) {
        error = "归档条目元数据过大";
        return false;
    }
    output.clear();
    put(output, encoded.size());
    put(output, size);
    output.insert(output.end(), encoded.begin(), encoded.end());
    return true;
}

bool decodeMetadata(int fd, std::vector<archive_format::Entry>& entries,
                    std::vector<uint64_t>& offsets, std::string& error) {
    entries.clear();
    offsets.clear();
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 16) {
        error = "自定义归档数据不完整";
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(info.st_size);
    uint8_t magic[8];
    if (!readAt(fd, 0, magic, sizeof(magic)) || std::memcmp(magic, "DBCS0001", 8)) {
        error = "自定义归档格式无效";
        return false;
    }
    uint64_t pos = 8;
    while (pos <= total && total - pos >= 8) {
        uint8_t raw[16];
        if (!readAt(fd, pos, raw, 8)) break;
        const uint64_t length = get(raw);
        pos += 8;
        if (length == 0) {
            if (pos == total) return true;
            error = "自定义归档结束后存在额外数据";
            return false;
        }
        if (length > maxMetadata || entries.size() >= 1000000 || total - pos < 8 ||
            !readAt(fd, pos, raw + 8, 8)) break;
        const uint64_t size = get(raw + 8);
        pos += 8;
        if (length > total - pos) break;
        std::vector<uint8_t> metadata(static_cast<size_t>(length));
        if (!readAt(fd, pos, metadata.data(), metadata.size())) break;
        pos += length;
        if (size > total - pos) break;
        std::vector<archive_format::Entry> one;
        if (!archive_format::decode(metadata, "", one, error) || one.size() != 1 ||
            !one[0].data.empty() || (one[0].file.type != 0 && size != 0)) {
            if (error.empty()) error = "自定义归档元数据无效";
            return false;
        }
        one[0].file.metadata.size = size;
        entries.push_back(std::move(one[0]));
        offsets.push_back(pos);
        pos += size;
    }
    error = "自定义归档数据截断或长度无效";
    return false;
}
} // namespace dbackup::archive_custom_stream
