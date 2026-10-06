// shaderprep: command-line front end.
//
//   shaderprep detect    <game-dir>
//   shaderprep install   <game-dir> [--exe <rel>] [--mode auto|full|safe] [--accept-anticheat-risk]
//                                   [--no-scan] [--block-at-boot]
//   shaderprep uninstall <game-dir>
//   shaderprep scan      <dir> -o <out.psodb> [--max-file-mb N]
//   shaderprep infer     <in.psodb>... -o <candidates.psodb>
//   shaderprep analyze   --truth <recorded.psodb> <prepared.psodb>...
//   shaderprep report    <file.psodb>...
//   shaderprep merge     -o <out.psodb> <in.psodb>... [--as-imported]
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "analyze/analyze.hpp"
#include "core/hash.hpp"
#include "core/psodb.hpp"
#include "dxbc/dxbc.hpp"
#include "harvest/harvest.hpp"
#include "infer/infer.hpp"
#include "profile/profile.hpp"

using namespace sp;
namespace fs = std::filesystem;

namespace {

constexpr const char* kDataDir = "shaderprep";
constexpr const char* kInstallMarker = "installed.txt";

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;  // --key value / -o value
    std::vector<std::string> flags;           // --flag
    bool flag(const std::string& f) const {
        for (const auto& x : flags)
            if (x == f) return true;
        return false;
    }
    std::string opt(const std::string& k, const std::string& def = "") const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
};

Args parse_args(int argc, char** argv, int start, const std::vector<std::string>& valued) {
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string s = argv[i];
        bool takesValue = false;
        for (const auto& v : valued)
            if (v == s) takesValue = true;
        if (takesValue && i + 1 < argc)
            a.opts[s] = argv[++i];
        else if (s.size() > 1 && s[0] == '-')
            a.flags.push_back(s);
        else
            a.positional.push_back(s);
    }
    return a;
}

int usage() {
    std::puts(
        "shaderprep - pre-compiles a DX12 game's shaders before your first playthrough\n"
        "\n"
        "  detect    <game-dir>                 anti-cheat, engine and mode for a game\n"
        "  install   <game-dir> [options]       set up a game (full mode: drops the proxy DLL;\n"
        "                                       safe mode: pre-compiles out of process)\n"
        "      --exe <path>                     game executable, relative to <game-dir>\n"
        "      --mode auto|full|safe            default auto\n"
        "      --accept-anticheat-risk          allow full mode on an anti-cheat game\n"
        "      --block-at-boot                  hold the game at startup until compiling is done\n"
        "      --no-scan                        skip the disk scan\n"
        "  uninstall <game-dir>                 remove the proxy DLL (keeps recordings)\n"
        "  scan      <dir> -o <out.psodb>       harvest shaders from files on disk\n"
        "  infer     <in.psodb>... -o <out>     build candidate PSOs from shaders + learned style\n"
        "  analyze   --truth <rec.psodb> <prepared.psodb>...   coverage of a first-play run\n"
        "  report    <file.psodb>...            summarise database contents\n"
        "  merge     -o <out.psodb> <in.psodb>... [--as-imported]\n");
    return 2;
}

bool load_all(const std::vector<std::string>& paths, PsoDb& into, const PsoSource* as = nullptr) {
    for (const auto& p : paths) {
        PsoDb db;
        std::string err;
        if (!PsoDb::load(p, db, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return false;
        }
        if (!into.game.exe.empty() && !db.game.exe.empty() && !(into.game == db.game))
            std::fprintf(stderr, "warning: %s was recorded for a different game build (%s %s)\n", p.c_str(),
                         db.game.exe.c_str(), hex64(db.game.buildId).c_str());
        into.merge(db, as);
    }
    return true;
}

void print_report(const std::string& name, const PsoDb& db) {
    std::map<std::string, int> stages, origins, sources, apis;
    for (const auto& [h, b] : db.blobs()) {
        origins[origin_name(b.origin)]++;
        if (b.kind == BlobKind::Shader) {
            dxbc::ContainerInfo info;
            if (dxbc::parse(b.data.data(), b.data.size(), info))
                stages[std::string(dxbc::stage_name(info.stage)) + (info.dxil ? " (dxil)" : " (dxbc)")]++;
        }
    }
    int slow = 0, graphics = 0, compute = 0, other = 0;
    for (const auto& e : db.psos()) {
        sources[source_name(e.source)]++;
        apis[e.desc.api == PsoApi::Graphics ? "graphics desc" : e.desc.api == PsoApi::Compute ? "compute desc" : "stream"]++;
        if (e.maxCreateMs >= 2.0f) slow++;
        if (e.desc.is_compute()) compute++;
        else if (e.desc.is_graphics()) graphics++;
        else other++;
    }
    std::printf("%s\n", name.c_str());
    std::printf("  game:            %s (build %s)\n", db.game.exe.empty() ? "?" : db.game.exe.c_str(),
                hex64(db.game.buildId).c_str());
    std::printf("  shaders:         %zu\n", db.count_blobs(BlobKind::Shader));
    for (auto& [k, v] : stages) std::printf("    %-16s %d\n", k.c_str(), v);
    std::printf("  root signatures: %zu\n", db.count_blobs(BlobKind::RootSignature));
    std::printf("  blobs by origin:");
    for (auto& [k, v] : origins) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\n  PSOs:            %zu (graphics %d, compute %d, other %d)\n", db.psos().size(), graphics, compute, other);
    std::printf("    by source:    ");
    for (auto& [k, v] : sources) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\n    by api:       ");
    for (auto& [k, v] : apis) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\n    slow (>=2ms):  %d\n", slow);
}

int cmd_detect(const Args& a) {
    if (a.positional.empty()) return usage();
    const std::string& dir = a.positional[0];
    auto ac = detect_anticheat(dir);
    Profile p;
    std::printf("game folder:  %s\n", dir.c_str());
    std::printf("engine guess: %s\n", guess_engine(dir).c_str());
    std::printf("executable:   %s\n", guess_game_exe(dir).c_str());
    if (ac.empty()) {
        std::printf("anti-cheat:   none found\n");
    } else {
        for (const auto& f : ac) std::printf("anti-cheat:   %s (%s)\n", f.name.c_str(), f.path.c_str());
    }
    std::printf("mode:         %s\n", mode_name(resolve_mode(p, ac)));
    return 0;
}

int run_scan(const std::string& dir, const std::vector<std::string>& extra, const std::string& out,
             uint64_t maxFileMb, const GameIdentity* game) {
    PsoDb db;
    if (game) db.game = *game;
    Harvester h(db, BlobOrigin::Disk);
    DiskScanOptions opts;
    opts.maxFileSize = maxFileMb << 20;
    int lastPct = -1;
    opts.progress = [&](const std::string&, uint64_t done, uint64_t total) {
        int pct = total ? static_cast<int>(100 * done / total) : 100;
        if (pct != lastPct) {
            std::fprintf(stderr, "\rscanning %s ... %3d%%", dir.c_str(), pct);
            lastPct = pct;
        }
    };
    h.scan_directory(dir, opts);
    for (const auto& e : extra) h.scan_directory(e, opts);
    std::fprintf(stderr, "\n");
    const auto& s = h.stats();
    std::printf("scanned %llu files (%.1f GiB): %llu new shaders, %llu root signatures, %llu duplicates\n",
                (unsigned long long)s.files, s.bytesScanned / double(1ull << 30), (unsigned long long)s.newShaders,
                (unsigned long long)s.newRootSignatures, (unsigned long long)s.duplicates);
    if (s.newShaders == 0)
        std::printf("note: no loose shaders on disk. That is normal for games that pack and compress their\n"
                    "      archives; full mode finds them in memory instead.\n");
    if (!db.save(out)) {
        std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s\n", out.c_str());
    return 0;
}

int cmd_scan(const Args& a) {
    std::string out = a.opt("-o");
    if (a.positional.empty() || out.empty()) return usage();
    uint64_t maxMb = std::strtoull(a.opt("--max-file-mb", "0").c_str(), nullptr, 10);
    return run_scan(a.positional[0], {}, out, maxMb, nullptr);
}

int cmd_infer(const Args& a) {
    std::string out = a.opt("-o");
    if (a.positional.empty() || out.empty()) return usage();
    PsoDb db;
    if (!load_all(a.positional, db)) return 1;
    StyleModel style;
    style.learn(db);
    PsoDb cands;
    InferStats st = infer_candidates(db, style, cands);
    std::printf("learned %zu graphics styles, %zu compute styles\n", style.graphics_templates().size(),
                style.compute_templates().size());
    std::printf("candidates: %zu (compute exact %u, compute guessed %u, graphics guessed %u)\n", cands.psos().size(),
                st.computeExact, st.computeGuessed, st.graphicsGuessed);
    std::printf("pixel shaders with no compatible VS: %u, with no fitting style: %u, other stages skipped: %u%s\n",
                st.pixelShadersNoVs, st.pixelShadersNoTemplate, st.skippedStages,
                st.truncated ? " (candidate cap reached)" : "");
    if (style.graphics_templates().empty() && style.compute_templates().empty())
        std::printf("note: no recorded PSOs to learn from; only compute shaders with embedded root signatures\n"
                    "      can be pre-compiled. Record a short session (full mode) or import a recording.\n");
    if (!cands.save(out)) {
        std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s\n", out.c_str());
    return 0;
}

int cmd_analyze(const Args& a) {
    std::string truthPath = a.opt("--truth");
    if (truthPath.empty() || a.positional.empty()) return usage();
    PsoDb truth, prepared;
    if (!load_all({truthPath}, truth) || !load_all(a.positional, prepared)) return 1;
    std::fputs(analyze_coverage(truth, prepared).to_string().c_str(), stdout);
    return 0;
}

int cmd_report(const Args& a) {
    if (a.positional.empty()) return usage();
    for (const auto& p : a.positional) {
        PsoDb db;
        std::string err;
        if (!PsoDb::load(p, db, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        print_report(p, db);
    }
    return 0;
}

int cmd_merge(const Args& a) {
    std::string out = a.opt("-o");
    if (out.empty() || a.positional.empty()) return usage();
    PsoDb db;
    PsoSource imported = PsoSource::Imported;
    if (!load_all(a.positional, db, a.flag("--as-imported") ? &imported : nullptr)) return 1;
    size_t dropped = db.drop_dangling();
    if (dropped) std::printf("dropped %zu PSOs with missing blobs\n", dropped);
    if (!db.save(out)) {
        std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
        return 1;
    }
    print_report(out, db);
    return 0;
}

fs::path self_dir(const char* argv0) {
    std::error_code ec;
    fs::path p = fs::canonical(u8_path(argv0), ec);
    if (ec) p = fs::absolute(u8_path(argv0), ec);
    return p.parent_path();
}

int cmd_install(const Args& a, const char* argv0) {
    if (a.positional.empty()) return usage();
    std::string gameDir = a.positional[0];
    Profile p;
    std::string exeRel = a.opt("--exe", guess_game_exe(gameDir));
    if (exeRel.empty()) {
        std::fprintf(stderr, "error: no game executable found; pass --exe <path relative to game folder>\n");
        return 1;
    }
    fs::path exePath = u8_path(gameDir) / u8_path(exeRel);
    if (!fs::exists(exePath)) {
        std::fprintf(stderr, "error: %s does not exist\n", path_u8(exePath).c_str());
        return 1;
    }
    fs::path exeDir = exePath.parent_path();
    fs::path data = exeDir / kDataDir;
    fs::create_directories(data / "import");

    // Keep an existing profile (the user may have edited it); otherwise start a new one.
    fs::path profilePath = data / "profile.toml";
    std::string err;
    if (fs::exists(profilePath) && !load_profile(path_u8(profilePath), p, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    p.exe = exeRel;
    if (p.name.empty()) p.name = path_u8(exePath.stem());
    if (p.engine.empty()) p.engine = guess_engine(gameDir);
    std::string mode = a.opt("--mode");
    if (mode == "full") p.mode = Mode::Full;
    else if (mode == "safe") p.mode = Mode::Safe;
    else if (mode == "auto") p.mode = Mode::Auto;
    else if (!mode.empty()) return usage();
    if (a.flag("--accept-anticheat-risk")) p.acceptAntiCheatRisk = true;
    if (a.flag("--block-at-boot")) p.blockAtBoot = true;

    auto ac = detect_anticheat(gameDir);
    Mode resolved = resolve_mode(p, ac);
    for (const auto& f : ac) std::printf("anti-cheat detected: %s (%s)\n", f.name.c_str(), f.path.c_str());
    if (!ac.empty() && p.mode == Mode::Full && !p.acceptAntiCheatRisk)
        std::printf("full mode was requested but this game has anti-cheat. Re-run with --accept-anticheat-risk\n"
                    "to load the proxy DLL anyway. The game may refuse to start, or worse. Using safe mode.\n");
    if (!ac.empty() && resolved == Mode::Full)
        std::printf("WARNING: full mode on an anti-cheat game. A DLL will be loaded into the game process.\n"
                    "         Play offline / single-player only. You accepted this risk.\n");
    std::printf("mode: %s\n", mode_name(resolved));

    std::string toml = profile_to_toml(p);
    if (!write_file_atomic(path_u8(profilePath), Bytes(toml.begin(), toml.end()))) {
        std::fprintf(stderr, "error: cannot write %s\n", path_u8(profilePath).c_str());
        return 1;
    }

    GameIdentity game = identify_game(path_u8(exePath));
    std::string diskDb = path_u8(data / "disk.psodb");
    if (!a.flag("--no-scan")) {
        std::vector<std::string> extra;
        for (const auto& e : p.extraScanPaths) extra.push_back(path_u8(u8_path(gameDir) / u8_path(e)));
        if (int rc = run_scan(gameDir, extra, diskDb, 0, &game)) return rc;
    }

    fs::path tools = self_dir(argv0);
    if (resolved == Mode::Full) {
        fs::path proxySrc = tools / "d3d12.dll";
        fs::path proxyDst = exeDir / "d3d12.dll";
        if (fs::exists(proxyDst) && !fs::exists(data / kInstallMarker)) {
            std::fprintf(stderr,
                         "error: %s already exists and is not ours (another mod such as ReShade?).\n"
                         "       Remove it or install shaderprep in safe mode.\n",
                         path_u8(proxyDst).c_str());
            return 1;
        }
        if (!fs::exists(proxySrc)) {
            std::fprintf(stderr, "error: %s not found next to shaderprep\n", path_u8(proxySrc).c_str());
            return 1;
        }
        std::error_code ec;
        fs::copy_file(proxySrc, proxyDst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "error: copying proxy DLL: %s\n", ec.message().c_str());
            return 1;
        }
        std::string marker = "d3d12.dll\n";
        write_file_atomic(path_u8(data / kInstallMarker), Bytes(marker.begin(), marker.end()));
        std::printf("installed proxy: %s\n", path_u8(proxyDst).c_str());
        std::printf("next: start the game and wait at the main menu while shaders compile in the background\n"
                    "      (progress in %s).\n", path_u8(data / "log.txt").c_str());
    } else {
        // Safe mode: nothing touches the game. Compile everything we have, out of process, now.
        fs::path helper = tools / "shaderprep-warm.exe";
        std::vector<std::string> inputs;
        for (const char* f : {"disk.psodb", "recorded.psodb", "harvest.psodb"})
            if (fs::exists(data / f)) inputs.push_back(path_u8(data / f));
        for (auto& e : fs::directory_iterator(data / "import"))
            if (e.path().extension() == ".psodb") inputs.push_back(path_u8(e.path()));
        if (!fs::exists(helper)) {
            std::printf("safe mode: run shaderprep-warm.exe with these files before playing:\n");
            for (const auto& i : inputs) std::printf("  %s\n", i.c_str());
            return 0;
        }
        std::string cmd = "\"\"" + path_u8(helper) + "\"";
        for (const auto& i : inputs) cmd += " \"" + i + "\"";
        cmd += "\"";
        std::printf("safe mode: pre-compiling out of process...\n");
        return std::system(cmd.c_str());
    }
    return 0;
}

int cmd_uninstall(const Args& a) {
    if (a.positional.empty()) return usage();
    std::string gameDir = a.positional[0];
    int removed = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(u8_path(gameDir), fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 5) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->path().filename() != kDataDir || !fs::exists(it->path() / kInstallMarker)) continue;
        fs::path proxy = it->path().parent_path() / "d3d12.dll";
        if (fs::remove(proxy, ec)) {
            std::printf("removed %s\n", path_u8(proxy).c_str());
            removed++;
        }
        fs::remove(it->path() / kInstallMarker, ec);
    }
    if (!removed) std::printf("no shaderprep proxy found under %s\n", gameDir.c_str());
    std::printf("recordings and profile kept in the shaderprep folder (delete it to remove everything)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    Args a = parse_args(argc, argv, 2, {"-o", "--exe", "--mode", "--truth", "--max-file-mb"});
    try {
        if (cmd == "detect") return cmd_detect(a);
        if (cmd == "install") return cmd_install(a, argv[0]);
        if (cmd == "uninstall") return cmd_uninstall(a);
        if (cmd == "scan") return cmd_scan(a);
        if (cmd == "infer") return cmd_infer(a);
        if (cmd == "analyze") return cmd_analyze(a);
        if (cmd == "report") return cmd_report(a);
        if (cmd == "merge") return cmd_merge(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
