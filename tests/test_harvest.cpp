#include <filesystem>
#include <fstream>
#include <random>

#include "fixtures.hpp"
#include "harvest/harvest.hpp"
#include "test.hpp"

using namespace sp;

namespace {
Bytes noise(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    Bytes b(n);
    for (auto& c : b) c = static_cast<uint8_t>(rng());
    return b;
}
}  // namespace

TEST_CASE("harvest: finds containers embedded in noise, like an unpacked archive in memory") {
    Bytes buf = noise(10000, 1);
    auto vs = fx::shader(dxbc::Stage::Vertex, {}, {}, false, 1);
    auto ps = fx::shader(dxbc::Stage::Pixel, {}, {}, true, 2);
    auto rs = fx::root_sig(5);
    buf.insert(buf.begin() + 1001, vs.begin(), vs.end());
    buf.insert(buf.begin() + 5003, ps.begin(), ps.end());
    buf.insert(buf.end(), rs.begin(), rs.end());
    // Fake magic that must not be accepted.
    const char fake[] = "DXBC this is not a shader";
    buf.insert(buf.begin() + 20, fake, fake + sizeof(fake));

    PsoDb db;
    Harvester h(db, BlobOrigin::Memory);
    h.scan_buffer(buf.data(), buf.size());
    CHECK(h.stats().newShaders == 2);
    CHECK(h.stats().newRootSignatures == 1);
    CHECK(db.count_blobs(BlobKind::Shader) == 2);

    // Scanning again only finds duplicates.
    h.scan_buffer(buf.data(), buf.size());
    CHECK(h.stats().newShaders == 2);
    CHECK(h.stats().duplicates == 3);
}

TEST_CASE("harvest: reports truncated container so streaming scans can resume") {
    auto vs = fx::shader(dxbc::Stage::Vertex, {}, {}, false, 1);
    Bytes buf = noise(100, 2);
    buf.insert(buf.end(), vs.begin(), vs.end());
    PsoDb db;
    Harvester h(db, BlobOrigin::Disk);
    size_t resume = h.scan_buffer(buf.data(), buf.size() - 5);
    CHECK(resume == 100);
    CHECK(h.stats().newShaders == 0);
}

TEST_CASE("harvest: directory scan finds shaders across files and chunk boundaries") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "shaderprep_harvest_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    auto vs = fx::shader(dxbc::Stage::Vertex, {}, {}, false, 11);
    auto cs = fx::shader(dxbc::Stage::Compute, {}, {}, true, 12);
    {
        // A container placed across the 32 MiB chunk boundary of the streaming reader.
        std::ofstream f(dir / "big.bin", std::ios::binary);
        Bytes pad((32u << 20) - 40, 0x11);
        f.write(reinterpret_cast<const char*>(pad.data()), pad.size());
        f.write(reinterpret_cast<const char*>(vs.data()), vs.size());
        Bytes tail(1000, 0x22);
        f.write(reinterpret_cast<const char*>(tail.data()), tail.size());
    }
    {
        std::ofstream f(dir / "sub" / "shaders.cache", std::ios::binary);
        f.write(reinterpret_cast<const char*>(cs.data()), cs.size());
    }
    {
        std::ofstream f(dir / "movie.bk2", std::ios::binary);  // skipped by extension
        f.write(reinterpret_cast<const char*>(cs.data()), cs.size());
    }
    PsoDb db;
    Harvester h(db, BlobOrigin::Disk);
    h.scan_directory(dir.string());
    CHECK(h.stats().files == 2);
    CHECK(h.stats().newShaders == 2);
    fs::remove_all(dir);
}
