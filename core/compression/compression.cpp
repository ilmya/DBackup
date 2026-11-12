// 提供备份数据压缩与解压能力
// 按算法编号处理字节流，分别实现 Huffman、LZ77 和 DEFLATE 的压缩与解压。
// 根据指定算法处理字节数据，并在解压时检查格式、数据长度和错误
#include "core/compression/compression.h"
#include <array>
#include <limits>
#include <queue>
#include <zlib.h>
namespace dbackup::compression {
namespace {
// 表示 Huffman 树节点及其频率和左右子节点。
struct Node {
    uint64_t f;
    int l, r, symbol;
};
using Item = std::pair<uint64_t, int>;
// 根据字节频率构造 Huffman 树，并返回根节点索引
int tree(const std::array<uint32_t, 256>& f, std::vector<Node>& n) {
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    for (int i = 0; i < 256; i++)
        if (f[i]) {
            n.push_back({f[i], -1, -1, i});
            q.push({f[i], int(n.size() - 1)});
        }
    while (q.size() > 1) {
        auto a = q.top();
        q.pop();
        auto b = q.top();
        q.pop();
        n.push_back({a.first + b.first, a.second, b.second, -1});
        q.push({a.first + b.first, int(n.size() - 1)});
    }
    return q.empty() ? -1 : q.top().second;
}
// 遍历 Huffman 树，为叶节点生成对应的变长位编码
void codes(int i, const std::vector<Node>& n, std::vector<uint8_t>& p,
           std::array<std::vector<uint8_t>, 256>& c) {
    if (n[i].symbol >= 0) {
        c[n[i].symbol] = p;
        return;
    }
    p.push_back(0);
    codes(n[i].l, n, p, c);
    p.back() = 1;
    codes(n[i].r, n, p, c);
    p.pop_back();
}
// 写入频率表和 Huffman 位流，编码输入字节序列
bool henc(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (in.size() > UINT32_MAX)
        return false;
    std::array<uint32_t, 256> f{};
    for (auto b : in)
        f[b]++;
    for (auto x : f)
        for (int i = 0; i < 4; i++)
            out.push_back(x >> (8 * i));
    std::vector<Node> n;
    int root = tree(f, n);
    if (root < 0)
        return true;
    std::array<std::vector<uint8_t>, 256> c;
    std::vector<uint8_t> p;
    codes(root, n, p, c);
    int bit = 0;
    for (auto b : in)
        for (auto x : c[b]) {
            if (!bit)
                out.push_back(0);
            out.back() |= x << bit;
            bit = (bit + 1) % 8;
        }
    return true;
}
// 读取 Huffman 频率表并解码到指定长度，拒绝截断或超长数据
bool hdec(const std::vector<uint8_t>& in, size_t expected, std::vector<uint8_t>& out) {
    if (in.size() < 1024)
        return false;
    std::array<uint32_t, 256> f{};
    uint64_t sum = 0;
    size_t p = 0;
    for (auto& x : f) {
        for (int i = 0; i < 4; i++)
            x |= uint32_t(in[p++]) << (8 * i);
        sum += x;
    }
    if (sum != expected)
        return false;
    std::vector<Node> n;
    int root = tree(f, n);
    if (root < 0)
        return in.size() == p;
    if (n[root].symbol >= 0) {
        if (in.size() != p)
            return false;
        out.assign(expected, n[root].symbol);
        return true;
    }
    int node = root;
    size_t bit = 0;
    while (out.size() < expected) {
        if (p + bit / 8 >= in.size())
            return false;
        node = ((in[p + bit / 8] >> (bit % 8)) & 1) ? n[node].r : n[node].l;
        bit++;
        if (n[node].symbol >= 0) {
            out.push_back(n[node].symbol);
            node = root;
        }
    }
    if (p + (bit + 7) / 8 != in.size() || (bit % 8 && (in.back() >> (bit % 8))))
        return false;
    std::array<uint32_t, 256> actual{};
    for (auto b : out)
        actual[b]++;
    return f == actual;
}
// 使用滑动窗口和前向匹配生成本项目的 LZ77 标记序列
bool lenc(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    std::array<std::vector<size_t>, 65536> chains;
    auto key = [&](size_t p) { return unsigned(in[p]) * 256 + unsigned(in[p + 1]); };
    size_t p = 0;
    while (p < in.size()) {
        size_t best = 0, dist = 0;
        if (p + 2 < in.size()) {
            auto& c = chains[key(p)];
            int examined = 0;
            for (auto i = c.rbegin(); i != c.rend() && examined < 64; ++i, ++examined) {
                size_t d = p - *i;
                if (d > 65535)
                    break;
                size_t len = 0;
                while (len < 258 && p + len < in.size() && in[p + len] == in[p + len - d])
                    len++;
                if (len >= 3 && len > best) {
                    best = len;
                    dist = d;
                    if (len == 258)
                        break;
                }
            }
        }
        size_t count = best ? best : 1;
        if (best) {
            out.insert(out.end(), {1, uint8_t(dist), uint8_t(dist >> 8), uint8_t(best - 3)});
        } else {
            out.push_back(0);
            out.push_back(in[p]);
        }
        for (size_t i = 0; i < count && p + i + 1 < in.size(); i++) {
            auto& c = chains[key(p + i)];
            c.push_back(p + i);
            if (c.size() > 128)
                c.erase(c.begin(), c.begin() + 64);
        }
        p += count;
    }
    return true;
}
// 展开 LZ77 标记并验证回溯距离、输出长度和输入完整性
bool ldec(const std::vector<uint8_t>& in, size_t expected, std::vector<uint8_t>& out) {
    size_t p = 0;
    while (p < in.size()) {
        uint8_t t = in[p++];
        if (t == 0) {
            if (p == in.size() || out.size() == expected)
                return false;
            out.push_back(in[p++]);
        } else if (t == 1) {
            if (in.size() - p < 3)
                return false;
            size_t d = in[p] | size_t(in[p + 1]) << 8, n = size_t(in[p + 2]) + 3;
            p += 3;
            if (!d || d > out.size() || n > expected - out.size())
                return false;
            for (size_t i = 0; i < n; i++)
                out.push_back(out[out.size() - d]);
        } else
            return false;
    }
    return out.size() == expected;
}
} // namespace
// 选择压缩算法处理输入，并在失败时返回错误
bool encode(Compression m, const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
            std::string& error) {
    out.clear();
    bool ok = false;
    switch (m) {
    case Compression::None:
        out = in;
        ok = true;
        break;
    case Compression::Huffman:
        ok = henc(in, out);
        break;
    case Compression::Lz77:
        ok = lenc(in, out);
        break;
    case Compression::Deflate:
        if (in.size() <= std::numeric_limits<uLong>::max()) {
            uLongf n = compressBound(in.size());
            out.resize(n);
            ok = compress2(out.data(), &n, in.data(), in.size(), 6) == Z_OK;
            if (ok)
                out.resize(n);
        }
        break;
    }
    if (!ok) {
        out.clear();
        error = "压缩失败";
    }
    return ok;
}
// 按算法解压数据并检查输出是否符合预期长度
bool decode(Compression m, const std::vector<uint8_t>& in, size_t expected,
            std::vector<uint8_t>& out, std::string& error) {
    if (expected > size_t(2) * 1024 * 1024 * 1024) {
        error = "解压大小超过限制";
        return false;
    }
    out.clear();
    bool ok = false;
    switch (m) {
    case Compression::None:
        out = in;
        ok = out.size() == expected;
        break;
    case Compression::Huffman:
        ok = hdec(in, expected, out);
        break;
    case Compression::Lz77:
        ok = ldec(in, expected, out);
        break;
    case Compression::Deflate:
        if (in.size() <= std::numeric_limits<uLong>::max()) {
            out.resize(expected ? expected : 1);
            uLongf n = out.size();
            uLong consumed = in.size();
            ok = uncompress2(out.data(), &n, in.data(), &consumed) == Z_OK && n == expected &&
                 consumed == in.size();
            if (ok)
                out.resize(expected);
        }
        break;
    }
    if (!ok) {
        out.clear();
        error = "压缩数据损坏或大小不匹配";
    }
    return ok;
}
} // namespace dbackup::compression
