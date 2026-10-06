#include "profile/profile.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <sstream>

#include "core/bytes.hpp"

namespace sp {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Minimal TOML subset: [section], key = "string" | number | true/false | [array of those].
struct Value {
    enum Kind { Str, Num, Bool, Arr } kind = Str;
    std::string str;
    double num = 0;
    bool b = false;
    std::vector<Value> arr;
};

bool parse_scalar(const std::string& t, Value& v) {
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        v.kind = Value::Str;
        std::string out;
        for (size_t i = 1; i + 1 < t.size(); ++i) {
            if (t[i] == '\\' && i + 2 < t.size()) {
                char n = t[++i];
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            } else {
                out += t[i];
            }
        }
        v.str = out;
        return true;
    }
    if (t == "true" || t == "false") {
        v.kind = Value::Bool;
        v.b = t == "true";
        return true;
    }
    try {
        size_t used = 0;
        v.num = std::stod(t, &used);
        v.kind = Value::Num;
        return used == t.size();
    } catch (...) {
        return false;
    }
}

// Splits on commas outside quotes.
std::vector<std::string> split_array(const std::string& inner) {
    std::vector<std::string> items;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (c == '"' && (i == 0 || inner[i - 1] != '\\')) quoted = !quoted;
        if (c == ',' && !quoted) {
            items.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) items.push_back(trim(cur));
    return items;
}

std::string strip_comment(const std::string& line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) quoted = !quoted;
        if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

}  // namespace

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Auto: return "auto";
        case Mode::Full: return "full";
        case Mode::Safe: return "safe";
    }
    return "auto";
}

bool parse_profile(const std::string& text, Profile& p, std::string* error) {
    std::map<std::string, Value> kv;
    std::istringstream in(text);
    std::string line, section;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(strip_comment(line));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') {
                if (error) *error = "line " + std::to_string(lineNo) + ": bad section header";
                return false;
            }
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "line " + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        Value v;
        if (!val.empty() && val.front() == '[') {
            if (val.back() != ']') {
                if (error) *error = "line " + std::to_string(lineNo) + ": arrays must be on one line";
                return false;
            }
            v.kind = Value::Arr;
            for (const auto& item : split_array(val.substr(1, val.size() - 2))) {
                Value e;
                if (!parse_scalar(item, e)) {
                    if (error) *error = "line " + std::to_string(lineNo) + ": bad array item";
                    return false;
                }
                v.arr.push_back(e);
            }
        } else if (!parse_scalar(val, v)) {
            if (error) *error = "line " + std::to_string(lineNo) + ": bad value";
            return false;
        }
        kv[section.empty() ? key : section + "." + key] = v;
    }

    auto str = [&](const char* k, std::string& dst) {
        auto it = kv.find(k);
        if (it != kv.end() && it->second.kind == Value::Str) dst = it->second.str;
    };
    auto boolean = [&](const char* k, bool& dst) {
        auto it = kv.find(k);
        if (it != kv.end() && it->second.kind == Value::Bool) dst = it->second.b;
    };
    auto u32 = [&](const char* k, uint32_t& dst) {
        auto it = kv.find(k);
        if (it != kv.end() && it->second.kind == Value::Num && it->second.num >= 0) dst = static_cast<uint32_t>(it->second.num);
    };
    str("game.name", p.name);
    str("game.exe", p.exe);
    str("game.engine", p.engine);
    std::string mode;
    str("mode.mode", mode);
    mode = lower(mode);
    if (mode == "full") p.mode = Mode::Full;
    else if (mode == "safe") p.mode = Mode::Safe;
    else if (mode.empty() || mode == "auto") p.mode = Mode::Auto;
    else {
        if (error) *error = "mode.mode must be auto, full or safe";
        return false;
    }
    boolean("mode.accept_anticheat_risk", p.acceptAntiCheatRisk);
    boolean("warm.block_at_boot", p.blockAtBoot);
    u32("warm.block_timeout_s", p.blockTimeoutS);
    u32("warm.threads", p.threads);
    boolean("record.enabled", p.record);
    if (auto it = kv.find("record.slow_ms"); it != kv.end() && it->second.kind == Value::Num)
        p.slowMs = static_cast<float>(it->second.num);
    boolean("harvest.memory_scan", p.memoryScan);
    if (auto it = kv.find("harvest.scan_delays_s"); it != kv.end() && it->second.kind == Value::Arr) {
        p.scanDelaysS.clear();
        for (const auto& e : it->second.arr)
            if (e.kind == Value::Num && e.num >= 0) p.scanDelaysS.push_back(static_cast<uint32_t>(e.num));
    }
    if (auto it = kv.find("harvest.extra_paths"); it != kv.end() && it->second.kind == Value::Arr) {
        p.extraScanPaths.clear();
        for (const auto& e : it->second.arr)
            if (e.kind == Value::Str) p.extraScanPaths.push_back(e.str);
    }
    u32("infer.max_root_sigs_per_cs", p.infer.maxRootSigsPerCs);
    u32("infer.max_vs_per_ps", p.infer.maxVsPerPs);
    u32("infer.max_templates_per_pair", p.infer.maxTemplatesPerPair);
    u32("infer.max_candidates", p.infer.maxCandidates);
    return true;
}

bool load_profile(const std::string& path, Profile& out, std::string* error) {
    Bytes data;
    if (!read_file(path, data)) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    return parse_profile(std::string(data.begin(), data.end()), out, error);
}

std::string profile_to_toml(const Profile& p) {
    std::ostringstream o;
    o << "# shaderprep game profile\n\n";
    o << "[game]\n";
    o << "name = " << quote(p.name) << "\n";
    o << "exe = " << quote(p.exe) << "\n";
    o << "engine = " << quote(p.engine) << "\n\n";
    o << "[mode]\n";
    o << "# auto: full mode unless anti-cheat is detected, then safe mode (never touches the game).\n";
    o << "mode = " << quote(mode_name(p.mode)) << "\n";
    o << "# Full mode on an anti-cheat game loads a DLL into it. The game may refuse to start or worse.\n";
    o << "accept_anticheat_risk = " << (p.acceptAntiCheatRisk ? "true" : "false") << "\n\n";
    o << "[warm]\n";
    o << "block_at_boot = " << (p.blockAtBoot ? "true" : "false") << "\n";
    o << "block_timeout_s = " << p.blockTimeoutS << "\n";
    o << "threads = " << p.threads << "\n\n";
    o << "[record]\n";
    o << "enabled = " << (p.record ? "true" : "false") << "\n";
    o << "slow_ms = " << p.slowMs << "\n\n";
    o << "[harvest]\n";
    o << "memory_scan = " << (p.memoryScan ? "true" : "false") << "\n";
    o << "scan_delays_s = [";
    for (size_t i = 0; i < p.scanDelaysS.size(); ++i) o << (i ? ", " : "") << p.scanDelaysS[i];
    o << "]\n";
    o << "extra_paths = [";
    for (size_t i = 0; i < p.extraScanPaths.size(); ++i) o << (i ? ", " : "") << quote(p.extraScanPaths[i]);
    o << "]\n\n";
    o << "[infer]\n";
    o << "max_root_sigs_per_cs = " << p.infer.maxRootSigsPerCs << "\n";
    o << "max_vs_per_ps = " << p.infer.maxVsPerPs << "\n";
    o << "max_templates_per_pair = " << p.infer.maxTemplatesPerPair << "\n";
    o << "max_candidates = " << p.infer.maxCandidates << "\n";
    return o.str();
}

std::vector<AntiCheatFinding> detect_anticheat(const std::string& installDir) {
    struct Rule {
        const char* name;
        const char* pattern;  // lower-case file/folder name; '*' suffix = prefix match
    };
    static const Rule kRules[] = {
        {"Easy Anti-Cheat", "easyanticheat"},
        {"Easy Anti-Cheat", "easyanticheat_eos"},
        {"Easy Anti-Cheat", "easyanticheat_x64.dll"},
        {"Easy Anti-Cheat", "easyanticheat_eos_setup.exe"},
        {"Easy Anti-Cheat", "start_protected_game.exe"},
        {"BattlEye", "battleye"},
        {"BattlEye", "beservice*"},
        {"BattlEye", "beclient*"},
        {"nProtect GameGuard", "gameguard"},
        {"XIGNCODE3", "xigncode*"},
        {"Denuvo Anti-Cheat", "denuvo-anti-cheat*"},
        {"Riot Vanguard", "vgk.sys"},
        {"ACE (Tencent)", "ace-base*"},
    };
    std::vector<AntiCheatFinding> out;
    std::error_code ec;
    // Anti-cheat ships next to the game; a shallow walk is enough and avoids crawling assets.
    for (auto it = fs::recursive_directory_iterator(u8_path(installDir), fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
        }
        std::string name = lower(path_u8(it->path().filename()));
        for (const Rule& r : kRules) {
            std::string pat = r.pattern;
            bool match = pat.back() == '*' ? name.rfind(pat.substr(0, pat.size() - 1), 0) == 0 : name == pat;
            if (!match) continue;
            bool dup = std::any_of(out.begin(), out.end(), [&](const AntiCheatFinding& f) { return f.name == r.name; });
            if (!dup) out.push_back({r.name, path_u8(it->path())});
        }
    }
    return out;
}

Mode resolve_mode(const Profile& p, const std::vector<AntiCheatFinding>& ac) {
    if (p.mode == Mode::Safe) return Mode::Safe;
    if (ac.empty()) return Mode::Full;
    if (p.mode == Mode::Full && p.acceptAntiCheatRisk) return Mode::Full;
    return Mode::Safe;
}

std::string guess_engine(const std::string& installDir) {
    bool pak = false, utoc = false, engineDir = false, unity = false, reChunk = false, bdt = false;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(u8_path(installDir), fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        std::string name = lower(path_u8(it->path().filename()));
        std::string ext = lower(path_u8(it->path().extension()));
        if (ext == ".pak") pak = true;
        if (ext == ".utoc" || ext == ".ucas") utoc = true;
        if (name == "engine" && it->is_directory(ec)) engineDir = true;
        if (name == "unityplayer.dll" || name == "globalgamemanagers") unity = true;
        if (name.rfind("re_chunk_000", 0) == 0) reChunk = true;
        if (ext == ".bdt" || ext == ".bhd" || ext == ".dcx") bdt = true;
    }
    if (reChunk) return "re-engine";
    if (utoc || (pak && engineDir)) return "unreal";
    if (unity) return "unity";
    if (bdt) return "fromsoftware";
    return "unknown";
}

std::string guess_game_exe(const std::string& installDir) {
    static const char* kNotGames[] = {"unins", "crashreport", "launcher", "setup", "redist", "vc_redist",
                                      "dxsetup", "easyanticheat", "start_protected_game", "ue4prereq",
                                      "uninstall", "cefprocess", "helper", "battleye", "beservice"};
    std::string best;
    uint64_t bestSize = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(u8_path(installDir), fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        if (lower(path_u8(it->path().extension())) != ".exe") continue;
        std::string name = lower(path_u8(it->path().filename()));
        bool skip = false;
        for (const char* n : kNotGames)
            if (name.find(n) != std::string::npos) skip = true;
        if (skip) continue;
        uint64_t size = it->file_size(ec);
        if (!ec && size > bestSize) {
            bestSize = size;
            best = path_u8(fs::relative(it->path(), u8_path(installDir), ec));
        }
    }
    return best;
}

}  // namespace sp
