#include "analyze/analyze.hpp"
#include "fixtures.hpp"
#include "infer/infer.hpp"
#include "test.hpp"

using namespace sp;
using dxbc::Stage;

namespace {

constexpr uint32_t F3 = pod::DXGI_FORMAT_R32G32B32_FLOAT;
constexpr uint32_t F2 = pod::DXGI_FORMAT_R32G32_FLOAT;
constexpr uint32_t RGBA8 = pod::DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t RGBA16F = pod::DXGI_FORMAT_R16G16B16A16_FLOAT;

std::vector<dxbc::SigElement> vs_in() { return {fx::el("POSITION", 0, 0), fx::el("TEXCOORD", 0, 1)}; }
std::vector<dxbc::SigElement> vs_out() {
    return {fx::el("SV_Position", 0, 0, dxbc::D3D_NAME_POSITION), fx::el("TEXCOORD", 0, 1)};
}
std::vector<dxbc::SigElement> ps_in() {
    return {fx::el("SV_Position", 0, 0, dxbc::D3D_NAME_POSITION), fx::el("TEXCOORD", 0, 1)};
}
std::vector<dxbc::SigElement> ps_out(int targets) {
    std::vector<dxbc::SigElement> o;
    for (int i = 0; i < targets; ++i) o.push_back(fx::el("SV_Target", i, i, dxbc::D3D_NAME_TARGET));
    return o;
}
std::vector<InputElement> layout() { return {fx::ie("POSITION", 0, F3, 0), fx::ie("TEXCOORD", 0, F2, 12)}; }

}  // namespace

TEST_CASE("infer: signature helpers") {
    dxbc::ContainerInfo vs, ps, ps2;
    auto vsb = fx::shader(Stage::Vertex, vs_in(), vs_out(), false, 1);
    auto psb = fx::shader(Stage::Pixel, ps_in(), ps_out(2), false, 2);
    auto ps2b = fx::shader(Stage::Pixel, {fx::el("COLOR", 0, 1)}, ps_out(1), false, 3);
    REQUIRE(dxbc::parse(vsb.data(), vsb.size(), vs));
    REQUIRE(dxbc::parse(psb.data(), psb.size(), ps));
    REQUIRE(dxbc::parse(ps2b.data(), ps2b.size(), ps2));
    CHECK(vs_feeds_ps(vs, ps));
    CHECK(!vs_feeds_ps(vs, ps2));
    CHECK(layout_covers_vs(layout(), vs));
    CHECK(!layout_covers_vs({fx::ie("POSITION", 0, F3, 0)}, vs));
    CHECK(ps_render_target_count(ps) == 2);
}

TEST_CASE("infer: compute shader with embedded root signature becomes an exact candidate") {
    PsoDb db;
    auto rts0 = fx::rts0_payload(4);
    uint64_t cs = db.add_blob(BlobKind::Shader, BlobOrigin::Memory, fx::shader(Stage::Compute, {}, {}, true, 1, &rts0));
    // The game's recorded root signature with the same RTS0 payload must be reused.
    uint64_t rs = db.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded,
                              dxbc::build_container({{fourcc('R', 'T', 'S', '0'), rts0}}));
    uint64_t otherCs = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Compute, {}, {}, true, 2, &rts0));
    db.add_pso(fx::compute_pso(rs, otherCs), PsoSource::Recorded);

    StyleModel style;
    style.learn(db);
    PsoDb out;
    InferStats st = infer_candidates(db, style, out);
    CHECK(st.computeExact == 1);
    REQUIRE(out.psos().size() == 1);
    const PsoDesc& d = out.psos()[0].desc;
    CHECK(d.shader(SO_CS) == cs);
    CHECK(d.shader(SO_ROOT_SIGNATURE) == rs);
    CHECK(out.blob(cs) != nullptr);
    CHECK(out.blob(rs) != nullptr);
}

TEST_CASE("infer: compute shader without root signature tries learned compute root signatures") {
    PsoDb db;
    uint64_t rsA = db.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded, fx::root_sig(1));
    uint64_t rsB = db.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded, fx::root_sig(2));
    uint64_t cs1 = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Compute, {}, {}, true, 1));
    uint64_t cs2 = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Compute, {}, {}, true, 2));
    uint64_t cs3 = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Compute, {}, {}, true, 3));
    uint64_t fresh = db.add_blob(BlobKind::Shader, BlobOrigin::Memory, fx::shader(Stage::Compute, {}, {}, true, 4));
    db.add_pso(fx::compute_pso(rsA, cs1), PsoSource::Recorded);
    db.add_pso(fx::compute_pso(rsA, cs2), PsoSource::Recorded);
    db.add_pso(fx::compute_pso(rsB, cs3), PsoSource::Recorded);

    StyleModel style;
    style.learn(db);
    REQUIRE(style.compute_templates().size() == 2);
    CHECK(style.compute_templates()[0].style.shader(SO_ROOT_SIGNATURE) == rsA);  // most used first

    PsoDb out;
    InferOptions opts;
    opts.maxRootSigsPerCs = 1;
    InferStats st = infer_candidates(db, style, out, opts);
    CHECK(st.computeGuessed == 1);
    REQUIRE(out.psos().size() == 1);
    CHECK(out.psos()[0].desc.shader(SO_CS) == fresh);
    CHECK(out.psos()[0].desc.shader(SO_ROOT_SIGNATURE) == rsA);
}

TEST_CASE("infer: new pixel shader pairs with recorded VS and reuses its style") {
    PsoDb db;
    uint64_t rs = db.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded, fx::root_sig(1));
    uint64_t vs = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Vertex, vs_in(), vs_out(), true, 1));
    uint64_t psOld = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Pixel, ps_in(), ps_out(1), true, 2));
    uint64_t psNew = db.add_blob(BlobKind::Shader, BlobOrigin::Memory, fx::shader(Stage::Pixel, ps_in(), ps_out(1), true, 3));
    uint64_t psMrt = db.add_blob(BlobKind::Shader, BlobOrigin::Memory, fx::shader(Stage::Pixel, ps_in(), ps_out(2), true, 4));
    // Unrelated PS that no VS can feed.
    db.add_blob(BlobKind::Shader, BlobOrigin::Memory,
                fx::shader(Stage::Pixel, {fx::el("NORMAL", 0, 2)}, ps_out(1), true, 5));
    db.add_pso(fx::graphics_pso(rs, vs, psOld, layout(), {RGBA8}), PsoSource::Recorded);
    // A G-buffer style (2 targets) seen with a different VS.
    uint64_t vs2 = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Vertex, vs_in(), vs_out(), true, 6));
    uint64_t psG = db.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Pixel, ps_in(), ps_out(2), true, 7));
    db.add_pso(fx::graphics_pso(rs, vs2, psG, layout(), {RGBA8, RGBA16F}, 1), PsoSource::Recorded);

    StyleModel style;
    style.learn(db);
    CHECK(style.graphics_templates().size() == 2);

    PsoDb out;
    InferStats st = infer_candidates(db, style, out);
    CHECK(st.pixelShadersNoVs == 1);
    bool newWithVs = false, mrtWithGbuffer = false;
    for (const auto& e : out.psos()) {
        const PsoDesc& d = e.desc;
        if (d.shader(SO_PS) == psNew && d.shader(SO_VS) == vs && d.num_render_targets() == 1) newWithVs = true;
        if (d.shader(SO_PS) == psMrt && d.num_render_targets() == 2) mrtWithGbuffer = true;
        // Never pairs a 2-target PS with a 1-target style.
        if (d.shader(SO_PS) == psMrt) CHECK(d.num_render_targets() == 2);
    }
    CHECK(newWithVs);
    CHECK(mrtWithGbuffer);

    // Coverage: pretend the game later created psNew with vs and the first style.
    PsoDb truth;
    truth.merge(db);
    truth.add_pso(fx::graphics_pso(rs, vs, psNew, layout(), {RGBA8}), PsoSource::Recorded, 30.0f);
    // ...and one with a blend state we never saw.
    truth.add_pso(fx::graphics_pso(rs, vs, psNew, layout(), {RGBA8}, 9), PsoSource::Recorded, 30.0f);
    PsoDb prepared;
    prepared.merge(db);
    prepared.merge(out);
    CoverageReport rep = analyze_coverage(truth, prepared);
    CHECK(rep.truthPsos == 4);
    CHECK(rep.exactHits == 3);
    CHECK(rep.missStateMismatch == 1);
    CHECK(rep.mismatchFields["blend"] == 1);
    CHECK(rep.slowTruth == 2 && rep.slowCovered == 1);
}

TEST_CASE("analyze: root signatures compare by RTS0 payload") {
    auto rts0 = fx::rts0_payload(3);
    PsoDb truth, prepared;
    uint64_t cs = truth.add_blob(BlobKind::Shader, BlobOrigin::Recorded, fx::shader(Stage::Compute, {}, {}, true, 1, &rts0));
    uint64_t rsStandalone = truth.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded,
                                           dxbc::build_container({{fourcc('R', 'T', 'S', '0'), rts0}}));
    truth.add_pso(fx::compute_pso(rsStandalone, cs), PsoSource::Recorded);
    // Prepared side used the shader blob itself as the root signature source.
    prepared.add_blob(BlobKind::Shader, BlobOrigin::Memory, *&truth.blob(cs)->data);
    prepared.add_pso(fx::compute_pso(cs, cs), PsoSource::Inferred);
    CHECK(analyze_coverage(truth, prepared).exactHits == 1);
}
