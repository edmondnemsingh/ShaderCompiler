#include "core/psodb.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "core/hash.hpp"

namespace sp {

namespace {
constexpr uint32_t kMagic = fourcc('S', 'P', 'D', 'B');
constexpr uint32_t kVersion = 1;
}  // namespace

const char* origin_name(BlobOrigin o) {
    switch (o) {
        case BlobOrigin::Recorded: return "recorded";
        case BlobOrigin::Disk: return "disk";
        case BlobOrigin::Memory: return "memory";
        case BlobOrigin::Engine: return "engine";
    }
    return "unknown";
}

uint64_t PsoDb::add_blob(BlobKind kind, BlobOrigin origin, Bytes data) {
    uint64_t h = hash64(data.data(), data.size());
    auto it = blobs_.find(h);
    if (it == blobs_.end()) {
        blobs_.emplace(h, Blob{kind, origin, std::move(data)});
    } else if (static_cast<uint8_t>(origin) < static_cast<uint8_t>(it->second.origin)) {
        // Keep the most trustworthy origin (recorded beats harvested).
        it->second.origin = origin;
    }
    return h;
}

const Blob* PsoDb::blob(uint64_t hash) const {
    auto it = blobs_.find(hash);
    return it == blobs_.end() ? nullptr : &it->second;
}

bool PsoDb::add_pso(PsoDesc desc, PsoSource source, float createMs, uint32_t seen) {
    desc.normalize();
    uint64_t h = desc.hash();
    auto it = index_.find(h);
    if (it != index_.end()) {
        PsoEntry& e = psos_[it->second];
        e.seen += seen;
        e.maxCreateMs = std::max(e.maxCreateMs, createMs);
        if (static_cast<uint8_t>(source) < static_cast<uint8_t>(e.source)) e.source = source;
        return false;
    }
    index_.emplace(h, psos_.size());
    psos_.push_back(PsoEntry{std::move(desc), h, source, seen, createMs});
    return true;
}

const PsoEntry* PsoDb::pso(uint64_t hash) const {
    auto it = index_.find(hash);
    return it == index_.end() ? nullptr : &psos_[it->second];
}

void PsoDb::merge(const PsoDb& other, const PsoSource* as) {
    if (game.exe.empty()) game = other.game;
    for (const auto& [h, b] : other.blobs_) {
        auto it = blobs_.find(h);
        if (it == blobs_.end())
            blobs_.emplace(h, b);
        else if (static_cast<uint8_t>(b.origin) < static_cast<uint8_t>(it->second.origin))
            it->second.origin = b.origin;
    }
    for (const auto& e : other.psos_) add_pso(e.desc, as ? *as : e.source, e.maxCreateMs, e.seen);
}

size_t PsoDb::drop_dangling() {
    std::vector<PsoEntry> kept;
    size_t dropped = 0;
    for (auto& e : psos_) {
        bool ok = true;
        for (const auto& s : e.desc.subobjects)
            if (s.blob && !blobs_.count(s.blob)) ok = false;
        if (ok)
            kept.push_back(std::move(e));
        else
            ++dropped;
    }
    psos_ = std::move(kept);
    index_.clear();
    for (size_t i = 0; i < psos_.size(); ++i) index_.emplace(psos_[i].hash, i);
    return dropped;
}

size_t PsoDb::count_blobs(BlobKind kind) const {
    size_t n = 0;
    for (const auto& [h, b] : blobs_)
        if (b.kind == kind) ++n;
    return n;
}

Bytes PsoDb::serialize() const {
    ByteWriter w;
    w.u32(kMagic);
    w.u32(kVersion);
    w.str(game.exe);
    w.u64(game.buildId);
    w.str(game.label);

    // Sorted for deterministic output (stable diffs, reproducible hashes of shared files).
    std::vector<uint64_t> keys;
    keys.reserve(blobs_.size());
    for (const auto& [h, b] : blobs_) keys.push_back(h);
    std::sort(keys.begin(), keys.end());
    w.u32(static_cast<uint32_t>(keys.size()));
    for (uint64_t h : keys) {
        const Blob& b = blobs_.at(h);
        w.u64(h);
        w.u8(static_cast<uint8_t>(b.kind));
        w.u8(static_cast<uint8_t>(b.origin));
        w.blob(b.data);
    }
    w.u32(static_cast<uint32_t>(psos_.size()));
    for (const auto& e : psos_) {
        w.u8(static_cast<uint8_t>(e.source));
        w.u32(e.seen);
        w.f32(e.maxCreateMs);
        e.desc.encode(w);
    }
    return w.take();
}

PsoDb PsoDb::deserialize(const Bytes& data) {
    ByteReader r(data);
    if (r.u32() != kMagic) throw FormatError("not a .psodb file");
    uint32_t version = r.u32();
    if (version != kVersion) throw FormatError("unsupported .psodb version " + std::to_string(version));
    PsoDb db;
    db.game.exe = r.str();
    db.game.buildId = r.u64();
    db.game.label = r.str();
    uint32_t nb = r.u32();
    for (uint32_t i = 0; i < nb; ++i) {
        uint64_t h = r.u64();
        uint8_t kind = r.u8();
        uint8_t origin = r.u8();
        Bytes b = r.blob();
        if (kind > 1 || origin > 3) throw FormatError("bad blob header");
        if (hash64(b.data(), b.size()) != h) throw FormatError("blob hash mismatch");
        db.blobs_.emplace(h, Blob{static_cast<BlobKind>(kind), static_cast<BlobOrigin>(origin), std::move(b)});
    }
    uint32_t np = r.u32();
    for (uint32_t i = 0; i < np; ++i) {
        uint8_t source = r.u8();
        if (source > 3) throw FormatError("bad pso source");
        uint32_t seen = r.u32();
        float ms = r.f32();
        PsoDesc d = PsoDesc::decode(r);
        db.add_pso(std::move(d), static_cast<PsoSource>(source), ms, seen);
    }
    return db;
}

bool PsoDb::save(const std::string& path) const { return write_file_atomic(path, serialize()); }

bool PsoDb::load(const std::string& path, PsoDb& out, std::string* error) {
    Bytes data;
    if (!read_file(path, data)) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    try {
        out = deserialize(data);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = path + ": " + e.what();
        return false;
    }
}

GameIdentity identify_game(const std::string& exePath) {
    GameIdentity id;
    auto p = u8_path(exePath);
    std::string name = path_u8(p.filename());
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    id.exe = name;
    id.label = name;

    std::ifstream f(p, std::ios::binary);
    if (!f) return id;
    f.seekg(0, std::ios::end);
    uint64_t size = static_cast<uint64_t>(f.tellg());
    constexpr uint64_t kChunk = 1 << 20;
    Bytes buf;
    auto read_at = [&](uint64_t off, uint64_t n) {
        Bytes b(static_cast<size_t>(n));
        f.seekg(static_cast<std::streamoff>(off));
        f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
        buf.insert(buf.end(), b.begin(), b.end());
    };
    read_at(0, std::min(size, kChunk));
    if (size > kChunk) read_at(size - std::min(size - kChunk, kChunk), std::min(size - kChunk, kChunk));
    id.buildId = hash64(buf.data(), buf.size(), size);
    return id;
}

}  // namespace sp
