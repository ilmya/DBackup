#include "core/archive_tar.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace dbackup::archive_tar {
namespace {
using Entry = archive_format::Entry;
// 将无符号数写成 tar 头字段要求的八进制文本。
bool number(uint8_t* p, size_t n, uint64_t v) {
    std::memset(p, '0', n);
    p[n - 1] = 0;
    for (size_t i = n - 1; i > 0 && v;) {
        p[--i] = '0' + (v & 7);
        v >>= 3;
    }
    return !v;
}
// 解析 tar 头字段中的八进制数，并检查溢出和非法字符。
bool readnum(const uint8_t* p, size_t n, uint64_t& v) {
    v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0))
        i++;
    for (; i < n && p[i] && p[i] != ' '; i++) {
        if (p[i] < '0' || p[i] > '7' || v > (UINT64_MAX >> 3))
            return false;
        v = (v << 3) + (p[i] - '0');
    }
    return true;
}
// 读取定长 NUL 结尾字段中的可见文本。
std::string field(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n && p[i])
        i++;
    return std::string(reinterpret_cast<const char*>(p), i);
}
// 将十进制文本转换为无符号整数并拒绝溢出。
bool decimal(const std::string& s, uint64_t& value) {
    if (s.empty())
        return false;
    value = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || value > (UINT64_MAX - uint64_t(c - '0')) / 10)
            return false;
        value = value * 10 + uint64_t(c - '0');
    }
    return true;
}
// 将字符串限制在 tar 字段容量内并复制到目标缓冲区。
void copy(uint8_t* p, size_t n, const std::string& s) {
    std::memcpy(p, s.data(), std::min(n, s.size()));
}
// 生成符合 PAX 规则的键值记录并计算自身长度前缀。
std::string paxline(const std::string& k, const std::string& v) {
    std::string payload = k + "=" + v + "\n";
    size_t n = payload.size() + 2;
    for (;;) {
        size_t next = payload.size() + std::to_string(n).size() + 1;
        if (next == n)
            break;
        n = next;
    }
    return std::to_string(n) + " " + payload;
}
// 将纳秒时间转换为 PAX 使用的秒和小数部分表示。
std::string mtime(int64_t ns) {
    int64_t sec = ns / 1000000000LL, rem = ns % 1000000000LL;
    if (rem < 0) {
        sec--;
        rem += 1000000000LL;
    }
    if (sec < 0 && rem) {
        return "-" + std::to_string(-sec - 1) + "." +
               std::to_string(1000000000LL - rem + 1000000000LL).substr(1);
    }
    return std::to_string(sec) + "." + std::to_string(rem + 1000000000LL).substr(1);
}
// 解析 PAX 时间戳并还原为纳秒，拒绝超出整数范围的值。
bool parseTime(const std::string& s, int64_t& result) {
    try {
        size_t pos = 0;
        bool neg = !s.empty() && s[0] == '-';
        if (neg)
            pos++;
        size_t dot = s.find('.', pos);
        std::string whole = s.substr(pos, dot - pos);
        if (whole.empty() || whole.find_first_not_of("0123456789") != std::string::npos)
            return false;
        uint64_t sec = std::stoull(whole);
        if (sec > uint64_t(INT64_MAX / 1000000000LL))
            return false;
        uint64_t frac = 0;
        if (dot != std::string::npos) {
            std::string part = s.substr(dot + 1);
            if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos)
                return false;
            if (part.size() > 9)
                part.resize(9);
            while (part.size() < 9)
                part += '0';
            frac = std::stoull(part);
        }
        uint64_t value = sec * 1000000000ULL + frac;
        if (value > uint64_t(INT64_MAX))
            return false;
        result = neg ? -int64_t(value) : int64_t(value);
        return true;
    } catch (...) {
        return false;
    }
}
// 构造一个 512 字节 POSIX tar 头并填入校验和与设备信息。
bool header(std::vector<uint8_t>& out, const std::string& path, char type, uint64_t mode,
            uint64_t uid, uint64_t gid, uint64_t size, uint64_t time, uint64_t major,
            uint64_t minor) {
    std::array<uint8_t, 512> h{};
    copy(h.data(), 100, path);
    if (!number(h.data() + 100, 8, mode & 07777) || !number(h.data() + 108, 8, uid) ||
        !number(h.data() + 116, 8, gid) || !number(h.data() + 124, 12, size) ||
        !number(h.data() + 136, 12, time) || !number(h.data() + 329, 8, major) ||
        !number(h.data() + 337, 8, minor))
        return false;
    std::memset(h.data() + 148, ' ', 8);
    h[156] = type;
    copy(h.data() + 257, 6, "ustar");
    h[262] = 0;
    h[263] = '0';
    h[264] = '0';
    unsigned sum = 0;
    for (auto c : h)
        sum += c;
    if (!number(h.data() + 148, 8, sum))
        return false;
    out.insert(out.end(), h.begin(), h.end());
    return true;
}
// 追加 tar 数据块并补齐到 512 字节边界。
void data(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    out.insert(out.end(), p, p + n);
    out.resize((out.size() + 511) / 512 * 512, 0);
}
// 解析 PAX 扩展属性，验证长度字段和每条记录的边界。
bool parsePax(const uint8_t* p, size_t n, std::map<std::string, std::string>& attrs) {
    size_t i = 0;
    while (i < n) {
        size_t begin = i;
        uint64_t len = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            if (len > n / 10 + 1)
                return false;
            len = len * 10 + p[i++] - '0';
        }
        if (i == begin || i >= n || p[i++] != ' ' || len > n - begin || len < i - begin + 2 ||
            p[begin + len - 1] != '\n')
            return false;
        std::string s(reinterpret_cast<const char*>(p + i), begin + len - 1 - i);
        auto eq = s.find('=');
        if (eq == std::string::npos || s.find('\0') != std::string::npos)
            return false;
        attrs[s.substr(0, eq)] = s.substr(eq + 1);
        i = begin + len;
    }
    return true;
}
} // namespace
// 将普通文件、目录和特殊节点编码为 POSIX pax tar 字节流。
bool encodeEntryHeaders(const Entry& e, uint64_t size, size_t index, std::vector<uint8_t>& out,
                        std::string& error) {
    out.clear();
        const auto& f = e.file;
        if (f.type > 7 || !linux_file::validRelativePath(f.relativePath) ||
            (f.type != 0 && size != 0)) {
            error = "tar 路径或类型无效";
            return false;
        }
        std::string pax =
            paxline("path", f.relativePath) + paxline("mtime", mtime(f.metadata.mtimeNs));
        if (f.type == 2 || f.type == 3)
            pax += paxline("linkpath", f.linkTarget);
        if (f.type == 7)
            pax += paxline("DBackup.type", "socket");
        constexpr uint64_t maxUstarSize = 077777777777ULL;
        if (size > maxUstarSize)
            pax += paxline("size", std::to_string(size));
        if (f.metadata.uid > 07777777)
            pax += paxline("uid", std::to_string(f.metadata.uid));
        if (f.metadata.gid > 07777777)
            pax += paxline("gid", std::to_string(f.metadata.gid));
        if (!header(out, "PaxHeader", 'x', 0644, 0, 0, pax.size(), 0, 0, 0)) {
            error = "tar 扩展头过大";
            return false;
        }
        data(out, reinterpret_cast<const uint8_t*>(pax.data()), pax.size());
        static const char types[] = {'0', '5', '2', '1', '6', '3', '4', '0'};
        if (!header(out, "entry-" + std::to_string(index), types[f.type], f.metadata.mode,
                    f.metadata.uid > 07777777 ? 0 : f.metadata.uid,
                    f.metadata.gid > 07777777 ? 0 : f.metadata.gid,
                    size > maxUstarSize ? 0 : size, 0, f.metadata.deviceMajor,
                    f.metadata.deviceMinor)) {
            error = "tar 元数据超出 ustar 范围";
            return false;
        }
    return true;
}
bool encode(const std::vector<Entry>& entries, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    size_t index = 0;
    for (const auto& e : entries) {
        std::vector<uint8_t> prefix;
        if (!encodeEntryHeaders(e, e.file.type == 0 ? e.data.size() : 0, index++, prefix, error))
            return false;
        out.insert(out.end(), prefix.begin(), prefix.end());
        if (e.file.type == 0 && !e.data.empty())
            data(out, e.data.data(), e.data.size());
    }
    out.resize(out.size() + 1024, 0);
    return true;
}
// 校验 tar 头、PAX 属性和数据边界后解码条目。
static bool decodeInput(const std::vector<uint8_t>* in, int fd, uint64_t inputSize,
                        std::vector<Entry>& entries, std::vector<uint64_t>* offsets,
                        std::string& error) {
    entries.clear();
    if (offsets)
        offsets->clear();
    auto readAt = [&](uint64_t position, uint8_t* output, size_t size) {
        if (position > inputSize || size > inputSize - position)
            return false;
        if (in) {
            if (size)
                std::memcpy(output, in->data() + position, size);
            return true;
        }
        while (size) {
            const ssize_t count = pread(fd, output, size, static_cast<off_t>(position));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            output += count;
            position += static_cast<size_t>(count);
            size -= static_cast<size_t>(count);
        }
        return true;
    };
    uint64_t p = 0;
    std::map<std::string, std::string> attrs, global;
    bool done = false;
    while (inputSize - p >= 512) {
        std::array<uint8_t, 512> headerBytes{};
        if (!readAt(p, headerBytes.data(), headerBytes.size())) {
            error = "读取 tar 头失败";
            return false;
        }
        const uint8_t* h = headerBytes.data();
        if (std::all_of(h, h + 512, [](uint8_t c) { return c == 0; })) {
            std::array<uint8_t, 512> endBytes{};
            if (inputSize - p < 1024 || !readAt(p + 512, endBytes.data(), endBytes.size()) ||
                !std::all_of(endBytes.begin(), endBytes.end(), [](uint8_t c) { return c == 0; })) {
                error = "tar 结束标记不完整";
                return false;
            }
            done = true;
            break;
        }
        uint64_t stored = 0, sum = 0;
        for (size_t i = 0; i < 512; i++)
            sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (!readnum(h + 148, 8, stored) || stored != sum) {
            error = "tar 头校验失败";
            return false;
        }
        uint64_t mode, uid, gid, size, sec, maj, min;
        if (!readnum(h + 100, 8, mode) || !readnum(h + 108, 8, uid) || !readnum(h + 116, 8, gid) ||
            !readnum(h + 124, 12, size) || !readnum(h + 136, 12, sec) ||
            !readnum(h + 329, 8, maj) || !readnum(h + 337, 8, min) || uid > UINT32_MAX ||
            gid > UINT32_MAX || maj > UINT32_MAX || min > UINT32_MAX ||
            size > inputSize - p - 512) {
            error = "tar 元数据无效";
            return false;
        }
        char type = h[156];
        if (type != 'x' && type != 'g') {
            auto values = global;
            for (const auto& kv : attrs)
                values[kv.first] = kv.second;
            for (const auto& key : {"uid", "gid", "size"}) {
                auto it = values.find(key);
                if (it == values.end())
                    continue;
                uint64_t value;
                if (!decimal(it->second, value)) {
                    error = "pax 数值无效";
                    return false;
                }
                if (std::strcmp(key, "uid") == 0)
                    uid = value;
                else if (std::strcmp(key, "gid") == 0)
                    gid = value;
                else
                    size = value;
            }
            if (uid > UINT32_MAX || gid > UINT32_MAX || size > inputSize - p - 512) {
                error = "pax 元数据超限";
                return false;
            }
        }
        p += 512;
        if (size > UINT64_MAX - 511) {
            error = "tar 数据长度超限";
            return false;
        }
        uint64_t pad = (size + 511) / 512 * 512;
        if (pad > inputSize - p) {
            error = "tar 数据截断";
            return false;
        }
        if (type == 'x' || type == 'g') {
            auto& target = type == 'x' ? attrs : global;
            if (size > 16u * 1024u * 1024u) {
                error = "pax 扩展头过大";
                return false;
            }
            std::vector<uint8_t> pax(static_cast<size_t>(size));
            if (!readAt(p, pax.data(), pax.size()) || !parsePax(pax.data(), pax.size(), target)) {
                error = "pax 扩展头无效";
                return false;
            }
            p += pad;
            continue;
        }
        std::string path = field(h, 100), prefix = field(h + 345, 155);
        if (!prefix.empty())
            path = prefix + "/" + path;
        auto values = global;
        values.insert(attrs.begin(), attrs.end());
        for (auto& kv : attrs)
            values[kv.first] = kv.second;
        attrs.clear();
        if (values.count("path"))
            path = values["path"];
        if (path == "./" || path == ".") {
            p += pad;
            continue;
        }
        while (path.rfind("./", 0) == 0)
            path.erase(0, 2);
        while (path.size() > 1 && path.back() == '/')
            path.pop_back();
        if (path == ".") {
            p += pad;
            continue;
        }
        if (!linux_file::validRelativePath(path) || entries.size() >= 1000000) {
            error = "tar 路径不安全或条目过多";
            return false;
        }
        Entry e;
        e.file.relativePath = path;
        e.file.linkTarget = values.count("linkpath") ? values["linkpath"] : field(h + 157, 100);
        mode_t kind = 0;
        switch (type) {
        case 0:
        case '0':
            e.file.type = 0;
            kind = S_IFREG;
            break;
        case '5':
            e.file.type = 1;
            kind = S_IFDIR;
            break;
        case '2':
            e.file.type = 2;
            kind = S_IFLNK;
            break;
        case '1':
            e.file.type = 3;
            kind = S_IFREG;
            break;
        case '6':
            e.file.type = 4;
            kind = S_IFIFO;
            break;
        case '3':
            e.file.type = 5;
            kind = S_IFCHR;
            break;
        case '4':
            e.file.type = 6;
            kind = S_IFBLK;
            break;
        default:
            error = "tar 含不支持的文件类型";
            return false;
        }
        if (values.count("DBackup.type")) {
            if (values["DBackup.type"] != "socket" || e.file.type != 0 || size) {
                error = "pax 特殊类型无效";
                return false;
            }
            e.file.type = 7;
            kind = S_IFSOCK;
        }
        if (e.file.type == 3)
            while (e.file.linkTarget.rfind("./", 0) == 0)
                e.file.linkTarget.erase(0, 2);
        if (e.file.type != 0 && size) {
            error = "tar 特殊文件包含数据";
            return false;
        }
        e.file.metadata.mode = kind | (mode & 07777);
        e.file.metadata.uid = uid;
        e.file.metadata.gid = gid;
        e.file.metadata.deviceMajor = maj;
        e.file.metadata.deviceMinor = min;
        if (values.count("mtime")) {
            if (!parseTime(values["mtime"], e.file.metadata.mtimeNs)) {
                error = "pax 时间无效";
                return false;
            }
        } else {
            if (sec > uint64_t(INT64_MAX / 1000000000LL)) {
                error = "tar 时间超限";
                return false;
            }
            e.file.metadata.mtimeNs = sec * 1000000000LL;
        }
        e.file.metadata.size = size;
        if (size && !offsets) {
            if (size > std::numeric_limits<size_t>::max()) {
                error = "tar 文件大小超限";
                return false;
            }
            e.data.resize(static_cast<size_t>(size));
            if (!readAt(p, e.data.data(), e.data.size())) {
                error = "读取 tar 数据失败";
                return false;
            }
        }
        if (offsets)
            offsets->push_back(p);
        entries.push_back(std::move(e));
        p += pad;
    }
    if (!done) {
        error = "tar 缺少结束标记";
        return false;
    }
    return true;
}
bool decode(const std::vector<uint8_t>& input, std::vector<Entry>& entries, std::string& error) {
    return decodeInput(&input, -1, input.size(), entries, nullptr, error);
}
bool decodeMetadata(int fd, std::vector<Entry>& entries, std::vector<uint64_t>& offsets,
                    std::string& error) {
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        error = "无法检查 tar 文件";
        return false;
    }
    return decodeInput(nullptr, fd, static_cast<uint64_t>(info.st_size), entries, &offsets, error);
}
} // namespace dbackup::archive_tar
