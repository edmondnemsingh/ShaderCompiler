// Per-game profile (profiles/<game>.toml, copied to <game>/shaderprep/profile.toml on install),
// plus install-folder inspection: anti-cheat detection and a best-effort engine guess.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "infer/infer.hpp"

namespace sp {

enum class Mode { Auto, Full, Safe };
const char* mode_name(Mode m);

struct Profile {
    // [game]
    std::string name;
    std::string exe;     // relative to the install folder
    std::string engine;  // free-form hint: unreal, re-engine, fromsoftware, unity, ...
    // [mode]
    Mode mode = Mode::Auto;
    bool acceptAntiCheatRisk = false;  // required for Full mode on an anti-cheat game
    // [warm]
    bool blockAtBoot = false;
    uint32_t blockTimeoutS = 900;
    uint32_t threads = 0;  // 0 = hardware threads - 1
    // [record]
    bool record = true;
    float slowMs = 2.0f;
    // [harvest]
    bool memoryScan = true;
    std::vector<uint32_t> scanDelaysS = {15, 45, 120, 300};
    std::vector<std::string> extraScanPaths;
    // [infer]
    InferOptions infer;
};

bool parse_profile(const std::string& text, Profile& out, std::string* error = nullptr);
bool load_profile(const std::string& path, Profile& out, std::string* error = nullptr);
std::string profile_to_toml(const Profile& p);

struct AntiCheatFinding {
    std::string name;  // e.g. "Easy Anti-Cheat"
    std::string path;  // file or folder that gave it away
};
std::vector<AntiCheatFinding> detect_anticheat(const std::string& installDir);

// Mode the tool will actually use for this install.
Mode resolve_mode(const Profile& p, const std::vector<AntiCheatFinding>& ac);

std::string guess_engine(const std::string& installDir);

// Picks the most likely game executable (largest .exe that is not a known launcher/helper).
std::string guess_game_exe(const std::string& installDir);

}  // namespace sp
