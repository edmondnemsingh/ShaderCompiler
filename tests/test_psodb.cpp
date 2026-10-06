#include <cstdio>
#include <filesystem>

#include "core/psodb.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace sp;

TEST_CASE("psodb: round trip preserves blobs, psos and identity") {
    PsoDb db;
    db.game = {"game.exe", 0x1234, "Game"};
    uint64_t vs = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(dxbc::Stage::Vertex, {}, {}, false, 1));
    uint64_t ps = db.add_blob(BlobKind::Shader, BlobOrigin::Memory, fx::shader(dxbc::Stage::Pixel, {}, {}, false, 2));
    uint64_t rs = db.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded, fx::root_sig(1));
    auto d = fx::graphics_pso(rs, vs, ps, {fx::ie("POSITION", 0, pod::DXGI_FORMAT_R32G32B32_FLOAT, 0)},
                              {pod::DXGI_FORMAT_R8G8B8A8_UNORM});
    d.get_or_add(SO_STREAM_OUTPUT).streamOutput.entries.push_back({0, "TEXCOORD", 1, 0, 4, 0});
    CHECK(db.add_pso(d, PsoSource::Recorded, 5.5f));
    CHECK(!db.add_pso(d, PsoSource::Recorded, 1.0f));  // duplicate bumps seen
    CHECK(db.psos().size() == 1);
    CHECK(db.psos()[0].seen == 2);
    CHECK(db.psos()[0].maxCreateMs == 5.5f);

    PsoDb back = PsoDb::deserialize(db.serialize());
    CHECK(back.game == db.game);
    CHECK(back.blobs().size() == 3);
    REQUIRE(back.psos().size() == 1);
    CHECK(back.psos()[0].hash == db.psos()[0].hash);
    CHECK(back.psos()[0].desc == db.psos()[0].desc);
    CHECK(back.psos()[0].desc.num_render_targets() == 1);
    CHECK(back.psos()[0].desc.render_target_format(0) == pod::DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(back.blob(ps)->origin == BlobOrigin::Memory);
}

TEST_CASE("psodb: rejects corrupted data") {
    PsoDb db;
    db.add_blob(BlobKind::Shader, BlobOrigin::Disk, fx::shader(dxbc::Stage::Vertex, {}, {}, false, 1));
    Bytes data = db.serialize();
    data[data.size() - 10] ^= 0xFF;  // inside the blob -> hash mismatch
    bool threw = false;
    try {
        PsoDb::deserialize(data);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("psodb: merge unions entries and sums counts") {
    PsoDb a, b;
    uint64_t cs = a.add_blob(BlobKind::Shader, BlobOrigin::Disk, fx::shader(dxbc::Stage::Compute, {}, {}, true, 1));
    uint64_t rs = a.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded, fx::root_sig(1));
    b.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(dxbc::Stage::Compute, {}, {}, true, 1));
    b.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded, fx::root_sig(1));
    a.add_pso(fx::compute_pso(rs, cs), PsoSource::Inferred);
    b.add_pso(fx::compute_pso(rs, cs), PsoSource::Recorded, 3.0f);
    a.merge(b);
    REQUIRE(a.psos().size() == 1);
    CHECK(a.psos()[0].seen == 2);
    CHECK(a.psos()[0].source == PsoSource::Recorded);  // more trusted source wins
    CHECK(a.blob(cs)->origin == BlobOrigin::Recorded);
}

TEST_CASE("psodb: hash ignores api and cached pso, sanitize zeroes padding") {
    auto d1 = fx::compute_pso(1, 2);
    auto d2 = d1;
    d2.api = PsoApi::Stream;
    d2.get_or_add(SO_CACHED_PSO).pod = Bytes(16, 0xAB);
    d2.normalize();
    CHECK(d1.hash() == d2.hash());

    Bytes blend(pod::kBlendDescSize, 0);
    Bytes dirty = blend;
    dirty[8 + 37] = 0xCC;  // padding after RT0 write mask
    sanitize_pod(SO_BLEND, dirty);
    CHECK(dirty == blend);
}

TEST_CASE("psodb: save/load and drop_dangling") {
    PsoDb db;
    db.add_pso(fx::compute_pso(111, 222), PsoSource::Recorded);  // blobs missing
    CHECK(db.drop_dangling() == 1);
    CHECK(db.psos().empty());

    auto path = (std::filesystem::temp_directory_path() / "shaderprep_test.psodb").string();
    uint64_t cs = db.add_blob(BlobKind::Shader, BlobOrigin::Disk, fx::shader(dxbc::Stage::Compute, {}, {}, true, 9));
    db.add_pso(fx::compute_pso(cs, cs), PsoSource::Inferred);
    REQUIRE(db.save(path));
    PsoDb back;
    std::string err;
    REQUIRE(PsoDb::load(path, back, &err));
    CHECK(back.psos().size() == 1);
    std::filesystem::remove(path);
}
