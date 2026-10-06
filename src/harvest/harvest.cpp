#include "harvest/harvest.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/hash.hpp"
#include "dxbc/dxbc.hpp"

namespace sp {

namespace {

// Header-only check used to decide whether a candidate is merely cut off by the buffer end.
size_t declared_size(const uint8_t* p, size_t avail) {
    if (avail < dxbc::kHeaderSize || load_u32(p) != dxbc::kMagic) return 0;
    if (load_u16(p + 20) != 1 || load_u16(p + 22) != 0) return 0;
    uint32_t size = load_u32(p + 24);
    uint32_t count = load_u32(p + 28);
    if (count == 0 || count > dxbc::kMaxParts || size > dxbc::kMaxContainerSize) return 0;
    if (size < dxbc::kHeaderSize + 4ull * count) return 0;
    return size;
}
}  // namespace

void Harvester::add_container(const uint8_t* p, size_t size) {
    ++stats_.containers;
    uint64_t h = hash64(p, size);
    if (!seen_.insert(h).second || db_.blob(h)) {
        ++stats_.duplicates;
        return;
    }
    dxbc::ContainerInfo info;
    if (!dxbc::parse(p, size, info)) return;
    if (info.kind == dxbc::ContainerKind::Shader) {
        db_.add_blob(BlobKind::Shader, origin_, Bytes(p, p + size));
        ++stats_.newShaders;
    } else if (info.kind == dxbc::ContainerKind::RootSignature) {
        db_.add_blob(BlobKind::RootSignature, origin_, Bytes(p, p + size));
        ++stats_.newRootSignatures;
    }
}

size_t Harvester::scan_buffer(const uint8_t* p, size_t n) {
    stats_.bytesScanned += n;
    size_t firstTruncated = n;
    size_t i = 0;
    while (i + 4 <= n) {
        const void* hit = std::memchr(p + i, 'D', n - i - 3);
        if (!hit) break;
        i = static_cast<size_t>(static_cast<const uint8_t*>(hit) - p);
        if (std::memcmp(p + i, "DXBC", 4) != 0) {
            ++i;
            continue;
        }
        size_t size = dxbc::probe(p + i, n - i);
        if (size) {
            add_container(p + i, size);
            i += size;
            continue;
        }
        size_t declared = declared_size(p + i, n - i);
        if (declared > n - i && firstTruncated == n) firstTruncated = i;
        ++i;
    }
    return firstTruncated;
}

void Harvester::scan_stream(uint64_t size, const ReadFn& read, size_t chunkSize) {
    if (buf_.size() < chunkSize) buf_.resize(chunkSize);
    size_t window = chunkSize;
    uint64_t off = 0;
    while (off < size) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(window, size - off));
        size_t got = read(off, buf_.data(), want);
        if (got == 0) break;
        size_t resume = scan_buffer(buf_.data(), got);
        if (off + got >= size) break;  // end of stream
        if (resume == 0) {
            // A container larger than the window starts right here: widen the window to fit it.
            size_t need = declared_size(buf_.data(), got);
            if (need > window) {
                if (buf_.size() < need) buf_.resize(need);
                window = need;
                continue;
            }
            off += 1;
            continue;
        }
        // Resume at a truncated container header so it is seen whole next time; otherwise overlap
        // 3 bytes so a magic split across the boundary is still found.
        off += std::max<size_t>(std::min(resume, got - 3), 1);
        window = chunkSize;
    }
}

bool Harvester::scan_file(const std::string& path) {
    std::ifstream f(u8_path(path), std::ios::binary);
    if (!f) return false;
    ++stats_.files;
    f.seekg(0, std::ios::end);
    uint64_t size = static_cast<uint64_t>(f.tellg());
    scan_stream(size, [&](uint64_t off, uint8_t* dst, size_t n) -> size_t {
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(f.gcount());
    });
    return true;
}

void Harvester::scan_directory(const std::string& dir, const DiskScanOptions& opts) {
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string, uint64_t>> files;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(u8_path(dir), fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            std::string name = path_u8(it->path().filename());
            if (std::find(opts.skipDirectories.begin(), opts.skipDirectories.end(), name) != opts.skipDirectories.end())
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        std::string ext = path_u8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(opts.skipExtensions.begin(), opts.skipExtensions.end(), ext) != opts.skipExtensions.end())
            continue;
        uint64_t size = it->file_size(ec);
        if (ec || size < dxbc::kHeaderSize) continue;
        if (opts.maxFileSize && size > opts.maxFileSize) continue;
        files.emplace_back(path_u8(it->path()), size);
        total += size;
    }
    std::sort(files.begin(), files.end());
    uint64_t done = 0;
    for (const auto& [path, size] : files) {
        if (opts.progress) opts.progress(path, done, total);
        scan_file(path);
        done += size;
    }
    if (opts.progress) opts.progress("", total, total);
}

}  // namespace sp
