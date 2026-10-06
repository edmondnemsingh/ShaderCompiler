#include "fixtures.hpp"
#include "test.hpp"

using namespace sp;

TEST_CASE("dxbc: parses legacy DXBC vertex shader signatures") {
    auto vs = fx::shader(dxbc::Stage::Vertex, {fx::el("POSITION", 0, 0), fx::el("TEXCOORD", 0, 1)},
                         {fx::el("SV_Position", 0, 0, dxbc::D3D_NAME_POSITION), fx::el("TEXCOORD", 0, 1)},
                         false, 1);
    REQUIRE(dxbc::probe(vs.data(), vs.size()) == vs.size());
    dxbc::ContainerInfo info;
    REQUIRE(dxbc::parse(vs.data(), vs.size(), info));
    CHECK(info.kind == dxbc::ContainerKind::Shader);
    CHECK(info.stage == dxbc::Stage::Vertex);
    CHECK(!info.dxil);
    CHECK(info.smMajor == 5 && info.smMinor == 1);
    REQUIRE(info.inputs.size() == 2);
    CHECK(info.inputs[1].semantic == "TEXCOORD");
    CHECK(info.inputs[1].reg == 1);
    REQUIRE(info.outputs.size() == 2);
    CHECK(info.outputs[0].systemValue == dxbc::D3D_NAME_POSITION);
    CHECK(!info.hasRootSignature);
}

TEST_CASE("dxbc: parses DXIL pixel shader with embedded root signature") {
    auto rts0 = fx::rts0_payload(7);
    auto ps = fx::shader(dxbc::Stage::Pixel, {fx::el("SV_Position", 0, 0, 1)},
                         {fx::el("SV_Target", 0, 0, dxbc::D3D_NAME_TARGET), fx::el("SV_Target", 1, 1, dxbc::D3D_NAME_TARGET)},
                         true, 2, &rts0);
    dxbc::ContainerInfo info;
    REQUIRE(dxbc::parse(ps.data(), ps.size(), info));
    CHECK(info.dxil);
    CHECK(info.stage == dxbc::Stage::Pixel);
    CHECK(info.smMajor == 6);
    CHECK(info.hasRootSignature);
    CHECK(info.rootSignaturePart == rts0);
    CHECK(info.outputs.size() == 2);
}

TEST_CASE("dxbc: standalone root signature container is classified") {
    auto rs = fx::root_sig(3);
    dxbc::ContainerInfo info;
    REQUIRE(dxbc::parse(rs.data(), rs.size(), info));
    CHECK(info.kind == dxbc::ContainerKind::RootSignature);
    CHECK(info.stage == dxbc::Stage::None);
}

TEST_CASE("dxbc: probe rejects corrupt or truncated containers") {
    auto vs = fx::shader(dxbc::Stage::Vertex, {}, {}, false, 1);
    CHECK(dxbc::probe(vs.data(), vs.size() - 1) == 0);  // truncated
    auto bad = vs;
    bad[20] = 2;  // version
    CHECK(dxbc::probe(bad.data(), bad.size()) == 0);
    bad = vs;
    bad[28] = 0;  // zero parts
    CHECK(dxbc::probe(bad.data(), bad.size()) == 0);
    bad = vs;
    uint32_t off = load_u32(bad.data() + 32);
    bad[off] = 0x01;  // non-ASCII part fourcc
    CHECK(dxbc::probe(bad.data(), bad.size()) == 0);
    // Container with only signature parts (no program, no root signature) is not interesting.
    auto sigOnly = dxbc::build_container({{fourcc('I', 'S', 'G', 'N'), dxbc::build_signature_part({}, 0)}});
    CHECK(dxbc::probe(sigOnly.data(), sigOnly.size()) == 0);
}

TEST_CASE("dxbc: compute and mesh stage detection") {
    auto cs = fx::shader(dxbc::Stage::Compute, {}, {}, true, 4);
    auto ms = fx::shader(dxbc::Stage::Mesh, {}, {}, true, 5);
    dxbc::ContainerInfo a, b;
    REQUIRE(dxbc::parse(cs.data(), cs.size(), a));
    REQUIRE(dxbc::parse(ms.data(), ms.size(), b));
    CHECK(a.stage == dxbc::Stage::Compute);
    CHECK(b.stage == dxbc::Stage::Mesh);
}
