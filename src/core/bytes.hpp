// Little-endian binary reader/writer used by every on-disk format in the project.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sp {

using Bytes = std::vector<uint8_t>;

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void blob(const Bytes& b) {
        u32(static_cast<uint32_t>(b.size()));
        raw(b.data(), b.size());
    }
    void raw(const void* p, size_t n) {
        auto* c = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), c, c + n);
    }
    const Bytes& data() const { return buf_; }
    Bytes take() { return std::move(buf_); }

private:
    Bytes buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit ByteReader(const Bytes& b) : ByteReader(b.data(), b.size()) {}

    uint8_t u8() { uint8_t v; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v; raw(&v, 8); return v; }
    float f32() { float v; raw(&v, 4); return v; }
    std::string str() {
        uint32_t len = u32();
        need(len);
        std::string s(reinterpret_cast<const char*>(p_ + off_), len);
        off_ += len;
        return s;
    }
    Bytes blob() {
        uint32_t len = u32();
        need(len);
        Bytes b(p_ + off_, p_ + off_ + len);
        off_ += len;
        return b;
    }
    void raw(void* dst, size_t n) {
        need(n);
        std::memcpy(dst, p_ + off_, n);
        off_ += n;
    }
    size_t remaining() const { return n_ - off_; }
    size_t offset() const { return off_; }

private:
    void need(size_t n) const {
        if (n > n_ - off_) throw FormatError("unexpected end of data");
    }
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
};

// Reads a little-endian u32 from an unaligned pointer.
inline uint32_t load_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint16_t load_u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
           (uint32_t(uint8_t(d)) << 24);
}

// UTF-8 string -> filesystem path (std::filesystem::u8path is deprecated in C++20).
inline std::filesystem::path u8_path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

inline std::string path_u8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool read_file(const std::string& path, Bytes& out);
bool write_file_atomic(const std::string& path, const Bytes& data);

}  // namespace sp
