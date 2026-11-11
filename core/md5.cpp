#include "core/md5.h"
#include <array>
#include <fstream>
#include <vector>
namespace dbackup::crypto {
namespace {
constexpr uint32_t k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr int shifts[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                            5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
class Md5 {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t length = 0;
    std::array<uint8_t, 64> buf{};
    size_t used = 0;
    void block(const uint8_t* p) {
        uint32_t m[16]{};
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 4; j++)
                m[i] |= uint32_t(p[i * 4 + j]) << (j * 8);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            uint32_t t = a + f + k[i] + m[g];
            a = d;
            d = c;
            c = b;
            b += ((t << shifts[i]) | (t >> (32 - shifts[i])));
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }

  public:
    void update(const uint8_t* p, size_t n) {
        length += n;
        for (size_t i = 0; i < n; i++) {
            buf[used++] = p[i];
            if (used == 64) {
                block(buf.data());
                used = 0;
            }
        }
    }
    std::string finish() {
        uint64_t bits = length * 8;
        uint8_t v = 0x80;
        update(&v, 1);
        v = 0;
        while (used != 56)
            update(&v, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++)
            l[i] = bits >> (8 * i);
        update(l, 8);
        static const char hex[] = "0123456789abcdef";
        std::string result;
        for (auto x : h)
            for (int j = 0; j < 4; j++) {
                uint8_t b = x >> (8 * j);
                result += hex[b >> 4];
                result += hex[b & 15];
            }
        return result;
    }
};
} // namespace
// 对内存数据执行 MD5 轮函数并返回小写十六进制摘要。
std::string md5(const uint8_t* data, size_t size) {
    Md5 m;
    if (size)
        m.update(data, size);
    return m.finish();
}
// 分块读取文件生成 MD5，避免将整个文件载入内存。
bool md5File(const std::string& path, std::string& digest, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法读取文件以计算 MD5";
        return false;
    }
    Md5 m;
    std::array<uint8_t, 65536> buf;
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), buf.size());
        m.update(buf.data(), size_t(f.gcount()));
    }
    if (!f.eof()) {
        error = "读取文件以计算 MD5 失败";
        return false;
    }
    digest = m.finish();
    return true;
}
} // namespace dbackup::crypto
