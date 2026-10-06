#include "infer/infer.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "core/hash.hpp"

namespace sp {

namespace {

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool starts_with_sv(const std::string& s) {
    return s.size() > 3 && (s[0] == 'S' || s[0] == 's') && (s[1] == 'V' || s[1] == 'v') && s[2] == '_';
}

// PS inputs the rasterizer generates itself; they never need a matching VS output.
bool ps_system_generated(const dxbc::SigElement& e) {
    static const char* kNames[] = {"SV_IsFrontFace", "SV_SampleIndex", "SV_PrimitiveID", "SV_Coverage",
                                   "SV_InnerCoverage", "SV_Barycentrics", "SV_ShadingRate", "SV_ViewID",
                                   "SV_StencilRef"};
    for (const char* n : kNames)
        if (iequals(e.semantic, n)) return true;
    return e.systemValue == dxbc::D3D_NAME_IS_FRONT_FACE || e.systemValue == dxbc::D3D_NAME_SAMPLE_INDEX ||
           e.systemValue == dxbc::D3D_NAME_PRIMITIVE_ID;
}

bool vs_system_input(const dxbc::SigElement& e) {
    return e.systemValue == dxbc::D3D_NAME_VERTEX_ID || e.systemValue == dxbc::D3D_NAME_INSTANCE_ID ||
           iequals(e.semantic, "SV_VertexID") || iequals(e.semantic, "SV_InstanceID");
}

const dxbc::SigElement* find_output(const dxbc::ContainerInfo& s, const dxbc::SigElement& in) {
    for (const auto& o : s.outputs)
        if (o.semanticIndex == in.semanticIndex && iequals(o.semantic, in.semantic)) return &o;
    return nullptr;
}

}  // namespace

uint64_t root_signature_key(const Bytes& blob) {
    auto [p, n] = dxbc::find_part(blob.data(), blob.size(), fourcc('R', 'T', 'S', '0'));
    if (!p) return 0;
    return hash64(p, n);
}

bool vs_feeds_ps(const dxbc::ContainerInfo& vs, const dxbc::ContainerInfo& ps) {
    for (const auto& in : ps.inputs) {
        if (ps_system_generated(in)) continue;
        const dxbc::SigElement* out = find_output(vs, in);
        if (!out) return false;
        // Legacy DXBC links by register; DXIL links by semantic, so only check registers there.
        if (!vs.dxil && !ps.dxil && out->reg != in.reg) return false;
    }
    return true;
}

bool layout_covers_vs(const std::vector<InputElement>& layout, const dxbc::ContainerInfo& vs) {
    for (const auto& in : vs.inputs) {
        if (vs_system_input(in)) continue;
        bool found = false;
        for (const auto& e : layout)
            if (e.semanticIndex == in.semanticIndex && iequals(e.semantic, in.semantic)) found = true;
        if (!found) return false;
    }
    return true;
}

uint32_t ps_render_target_count(const dxbc::ContainerInfo& ps) {
    uint32_t n = 0;
    for (const auto& o : ps.outputs) {
        bool target = o.systemValue == dxbc::D3D_NAME_TARGET || iequals(o.semantic, "SV_Target");
        if (!target) continue;
        uint32_t idx = ps.dxil ? o.semanticIndex : std::max(o.semanticIndex, o.reg);
        n = std::max(n, idx + 1);
    }
    return n;
}

size_t StyleModel::intern(std::vector<Template>& list, std::unordered_map<uint64_t, size_t>& index,
                          const PsoDesc& style) {
    uint64_t h = style.hash();
    auto it = index.find(h);
    if (it != index.end()) return it->second;
    index.emplace(h, list.size());
    list.push_back(Template{style, 0, 0, 0});
    return list.size() - 1;
}

void StyleModel::learn(const PsoDb& db) {
    for (const auto& e : db.psos()) {
        if (e.source != PsoSource::Recorded && e.source != PsoSource::Imported) continue;
        const PsoDesc& d = e.desc;
        if (uint64_t rs = d.shader(SO_ROOT_SIGNATURE)) {
            if (const Blob* b = db.blob(rs)) {
                uint64_t key = root_signature_key(b->data);
                if (key) rsByKey_.emplace(key, rs);
            }
        }
        PsoDesc style = d.without_shaders();
        if (d.is_compute()) {
            size_t t = intern(compute_, computeIndex_, style);
            compute_[t].count++;
        } else if (d.is_graphics()) {
            size_t t = intern(graphics_, graphicsIndex_, style);
            Template& tpl = graphics_[t];
            tpl.count++;
            if (d.shader(SO_PS))
                tpl.withPs++;
            else
                tpl.withoutPs++;
            for (const auto& s : d.subobjects) {
                if (!is_shader_subobject(s.type) || !s.blob) continue;
                auto& uses = byShader_[s.blob];
                auto u = std::find_if(uses.begin(), uses.end(), [&](auto& p) { return p.first == t; });
                if (u == uses.end())
                    uses.emplace_back(t, 1);
                else
                    u->second++;
            }
        }
    }
    for (auto& [shader, uses] : byShader_)
        std::stable_sort(uses.begin(), uses.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::stable_sort(compute_.begin(), compute_.end(),
                     [](const Template& a, const Template& b) { return a.count > b.count; });
    computeIndex_.clear();
    for (size_t i = 0; i < compute_.size(); ++i) computeIndex_.emplace(compute_[i].style.hash(), i);
    graphicsOrder_.resize(graphics_.size());
    for (size_t i = 0; i < graphics_.size(); ++i) graphicsOrder_[i] = i;
    std::stable_sort(graphicsOrder_.begin(), graphicsOrder_.end(),
                     [&](size_t a, size_t b) { return graphics_[a].count > graphics_[b].count; });
}

std::vector<size_t> StyleModel::templates_for_shader(uint64_t shader) const {
    std::vector<size_t> out;
    auto it = byShader_.find(shader);
    if (it != byShader_.end())
        for (const auto& [t, n] : it->second) out.push_back(t);
    return out;
}

uint32_t StyleModel::shader_use_count(uint64_t shader) const {
    auto it = byShader_.find(shader);
    if (it == byShader_.end()) return 0;
    uint32_t n = 0;
    for (const auto& [t, c] : it->second) n += c;
    return n;
}

uint64_t StyleModel::root_sig_blob_for_key(uint64_t key) const {
    auto it = rsByKey_.find(key);
    return it == rsByKey_.end() ? 0 : it->second;
}

InferStats infer_candidates(const PsoDb& db, const StyleModel& style, PsoDb& out, const InferOptions& opts) {
    InferStats st;
    if (out.game.exe.empty()) out.game = db.game;

    std::unordered_set<uint64_t> covered;
    for (const auto& e : db.psos())
        for (const auto& s : e.desc.subobjects)
            if (is_shader_subobject(s.type) && s.blob) covered.insert(s.blob);

    struct Shader {
        uint64_t hash;
        dxbc::ContainerInfo info;
    };
    std::vector<Shader> cs, vs, ps;
    std::vector<uint64_t> keys;
    for (const auto& [h, b] : db.blobs())
        if (b.kind == BlobKind::Shader) keys.push_back(h);
    std::sort(keys.begin(), keys.end());  // deterministic output
    for (uint64_t h : keys) {
        const Blob& b = *db.blob(h);
        Shader s{h, {}};
        if (!dxbc::parse(b.data.data(), b.data.size(), s.info)) continue;
        bool isCovered = covered.count(h) != 0;
        switch (s.info.stage) {
            case dxbc::Stage::Compute:
                if (!isCovered) cs.push_back(std::move(s));
                break;
            case dxbc::Stage::Vertex:
                vs.push_back(std::move(s));  // covered VS can still pair with new PS
                break;
            case dxbc::Stage::Pixel:
                if (!isCovered) ps.push_back(std::move(s));
                break;
            default:
                if (!isCovered) st.skippedStages++;
                break;
        }
    }
    st.shadersConsidered = static_cast<uint32_t>(cs.size() + ps.size());

    std::unordered_set<uint64_t> neededBlobs;
    auto emit = [&](PsoDesc d) {
        if (out.psos().size() >= opts.maxCandidates) {
            st.truncated = true;
            return false;
        }
        d.normalize();
        if (db.has_pso(d.hash())) return false;
        for (const auto& s : d.subobjects)
            if (s.blob) neededBlobs.insert(s.blob);
        return out.add_pso(std::move(d), PsoSource::Inferred);
    };
    auto with_shaders = [](PsoDesc d, std::initializer_list<std::pair<uint32_t, uint64_t>> shaders) {
        for (auto [type, h] : shaders) d.get_or_add(type).blob = h;
        return d;
    };

    // 1. Compute shaders that embed their root signature: fully determined, exact candidates.
    std::vector<const Shader*> csNoRs;
    for (const auto& s : cs) {
        if (!s.info.hasRootSignature) {
            csNoRs.push_back(&s);
            continue;
        }
        uint64_t key = hash64(s.info.rootSignaturePart.data(), s.info.rootSignaturePart.size());
        uint64_t rs = style.root_sig_blob_for_key(key);
        if (!rs) rs = s.hash;  // CreateRootSignature accepts a shader blob carrying RTS0
        PsoDesc d;
        if (!style.compute_templates().empty()) {
            d = style.compute_templates().front().style;
        } else {
            d.api = PsoApi::Compute;
            d.get_or_add(SO_NODE_MASK).pod = Bytes(4, 0);
            d.get_or_add(SO_FLAGS).pod = Bytes(4, 0);
        }
        d.get_or_add(SO_ROOT_SIGNATURE).blob = rs;
        if (emit(with_shaders(d, {{SO_CS, s.hash}}))) st.computeExact++;
    }

    // 2. Pixel shaders: pair with compatible vertex shaders, dress with learned styles.
    const auto& tpls = style.graphics_templates();
    for (const auto& p : ps) {
        std::vector<const Shader*> vsCands;
        for (const auto& v : vs)
            if (vs_feeds_ps(v.info, p.info)) vsCands.push_back(&v);
        if (vsCands.empty()) {
            st.pixelShadersNoVs++;
            continue;
        }
        // Prefer VS the game was seen using, then VS whose outputs match the PS inputs exactly.
        auto exactness = [&](const Shader* v) {
            size_t outs = 0;
            for (const auto& o : v->info.outputs)
                if (!starts_with_sv(o.semantic)) outs++;
            size_t ins = 0;
            for (const auto& i : p.info.inputs)
                if (!starts_with_sv(i.semantic)) ins++;
            return outs == ins ? 1 : 0;
        };
        std::stable_sort(vsCands.begin(), vsCands.end(), [&](const Shader* a, const Shader* b) {
            uint32_t ua = style.shader_use_count(a->hash), ub = style.shader_use_count(b->hash);
            if (ua != ub) return ua > ub;
            return exactness(a) > exactness(b);
        });
        if (vsCands.size() > opts.maxVsPerPs) vsCands.resize(opts.maxVsPerPs);

        uint32_t rtCount = ps_render_target_count(p.info);
        bool anyTemplate = false;
        for (const Shader* v : vsCands) {
            std::vector<size_t> chosen;
            auto consider = [&](size_t t, bool requireExactRt) {
                if (chosen.size() >= opts.maxTemplatesPerPair) return;
                if (std::find(chosen.begin(), chosen.end(), t) != chosen.end()) return;
                const PsoDesc& sd = tpls[t].style;
                if (tpls[t].withPs == 0) return;
                uint32_t n = sd.num_render_targets();
                if (requireExactRt ? n != rtCount : n < rtCount) return;
                const Subobject* il = sd.find(SO_INPUT_LAYOUT);
                static const std::vector<InputElement> kEmpty;
                if (!layout_covers_vs(il ? il->inputLayout : kEmpty, v->info)) return;
                chosen.push_back(t);
            };
            for (bool exact : {true, false}) {
                for (size_t t : style.templates_for_shader(v->hash)) consider(t, exact);
                for (size_t t : style.graphics_by_frequency()) consider(t, exact);
            }
            for (size_t t : chosen) {
                anyTemplate = true;
                if (emit(with_shaders(tpls[t].style, {{SO_VS, v->hash}, {SO_PS, p.hash}}))) st.graphicsGuessed++;
            }
        }
        if (!anyTemplate) st.pixelShadersNoTemplate++;
    }

    // 2b. Vertex shaders never seen in any PSO: try the depth-only (no PS) styles.
    for (const auto& v : vs) {
        if (covered.count(v.hash)) continue;
        uint32_t used = 0;
        for (size_t t : style.graphics_by_frequency()) {
            if (used >= opts.maxTemplatesPerPair) break;
            const auto& tpl = tpls[t];
            if (tpl.withoutPs == 0) continue;
            const Subobject* il = tpl.style.find(SO_INPUT_LAYOUT);
            static const std::vector<InputElement> kEmpty;
            if (!layout_covers_vs(il ? il->inputLayout : kEmpty, v.info)) continue;
            used++;
            if (emit(with_shaders(tpl.style, {{SO_VS, v.hash}}))) st.graphicsGuessed++;
        }
    }

    // 3. Compute shaders without an embedded root signature: try the game's compute root
    //    signatures, most used first. The runtime rejects mismatches before compiling.
    for (const Shader* s : csNoRs) {
        uint32_t n = 0;
        for (const auto& tpl : style.compute_templates()) {
            if (n >= opts.maxRootSigsPerCs) break;
            n++;
            if (emit(with_shaders(tpl.style, {{SO_CS, s->hash}}))) st.computeGuessed++;
        }
    }

    for (uint64_t h : neededBlobs)
        if (const Blob* b = db.blob(h)) out.add_blob(b->kind, b->origin, b->data);
    return st;
}

}  // namespace sp
