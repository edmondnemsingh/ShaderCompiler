#include "core/bytes.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace sp {

bool read_file(const std::string& path, Bytes& out) {
    std::ifstream f(u8_path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(f);
}

// Writes to "<path>.tmp" then renames, so a crash mid-write never leaves a torn database.
bool write_file_atomic(const std::string& path, const Bytes& data) {
    namespace fs = std::filesystem;
    fs::path target = u8_path(path);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        fs::rename(tmp, target, ec);
    }
    return !ec;
}

}  // namespace sp
