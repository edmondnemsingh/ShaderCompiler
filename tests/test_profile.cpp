#include <filesystem>
#include <fstream>

#include "profile/profile.hpp"
#include "test.hpp"

using namespace sp;
namespace fs = std::filesystem;

TEST_CASE("profile: parse and round trip") {
    const char* text = R"(
# comment
[game]
name = "Ace Combat 8"   # trailing comment
exe = "Game/Binaries/Win64/Game-Win64-Shipping.exe"
engine = "unreal"

[mode]
mode = "full"
accept_anticheat_risk = true

[warm]
block_at_boot = true
threads = 6

[harvest]
scan_delays_s = [5, 20]
extra_paths = ["C:\\Games\\x", "D:/y"]

[infer]
max_vs_per_ps = 2
)";
    Profile p;
    std::string err;
    REQUIRE(parse_profile(text, p, &err));
    CHECK(p.name == "Ace Combat 8");
    CHECK(p.engine == "unreal");
    CHECK(p.mode == Mode::Full);
    CHECK(p.acceptAntiCheatRisk);
    CHECK(p.blockAtBoot);
    CHECK(p.threads == 6);
    CHECK(p.scanDelaysS.size() == 2 && p.scanDelaysS[1] == 20);
    CHECK(p.extraScanPaths.size() == 2 && p.extraScanPaths[0] == "C:\\Games\\x");
    CHECK(p.infer.maxVsPerPs == 2);

    Profile q;
    REQUIRE(parse_profile(profile_to_toml(p), q, &err));
    CHECK(q.name == p.name);
    CHECK(q.exe == p.exe);
    CHECK(q.mode == p.mode);
    CHECK(q.acceptAntiCheatRisk == p.acceptAntiCheatRisk);
    CHECK(q.extraScanPaths == p.extraScanPaths);
    CHECK(q.scanDelaysS == p.scanDelaysS);
}

TEST_CASE("profile: rejects malformed input") {
    Profile p;
    CHECK(!parse_profile("[game\nname = \"x\"", p));
    CHECK(!parse_profile("name \"x\"", p));
    CHECK(!parse_profile("[mode]\nmode = \"turbo\"", p));
}

TEST_CASE("profile: anti-cheat detection and mode resolution") {
    fs::path dir = fs::temp_directory_path() / "shaderprep_ac_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "EasyAntiCheat");
    std::ofstream(dir / "start_protected_game.exe") << "x";
    std::ofstream(dir / "Game.exe") << "xxxxxxxx";
    auto ac = detect_anticheat(dir.string());
    REQUIRE(ac.size() == 1);
    CHECK(ac[0].name == "Easy Anti-Cheat");
    CHECK(guess_game_exe(dir.string()) == "Game.exe");

    Profile p;
    CHECK(resolve_mode(p, ac) == Mode::Safe);       // auto + anti-cheat -> safe
    CHECK(resolve_mode(p, {}) == Mode::Full);       // auto, clean -> full
    p.mode = Mode::Full;
    CHECK(resolve_mode(p, ac) == Mode::Safe);       // full needs explicit risk acceptance
    p.acceptAntiCheatRisk = true;
    CHECK(resolve_mode(p, ac) == Mode::Full);
    p.mode = Mode::Safe;
    CHECK(resolve_mode(p, {}) == Mode::Safe);
    fs::remove_all(dir);
}

TEST_CASE("profile: engine guess") {
    fs::path dir = fs::temp_directory_path() / "shaderprep_engine_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "natives");
    std::ofstream(dir / "re_chunk_000.pak") << "x";
    CHECK(guess_engine(dir.string()) == "re-engine");
    fs::remove_all(dir);
    fs::create_directories(dir / "Engine");
    fs::create_directories(dir / "Game" / "Content" / "Paks");
    std::ofstream(dir / "Game" / "Content" / "Paks" / "Game-Windows.utoc") << "x";
    CHECK(guess_engine(dir.string()) == "unreal");
    fs::remove_all(dir);
}
