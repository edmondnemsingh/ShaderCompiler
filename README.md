# shaderprep

Pre-compiles a DirectX 12 game's shaders **before your first playthrough**, so the stutter you
normally get the first time you see a new effect, area or enemy is gone, or at least greatly reduced.

It works with any engine (Unreal 5, RE Engine, FromSoftware, Unity and others). It does not need
to understand each game's file formats.

> Status: early. Everything builds, and the full chain (record → find shaders in memory → infer
> → pre-compile) passes an automated smoke test. It has **not been tested on real games yet**.
> The first real-game step is the measurement plan in [docs/DESIGN.md](docs/DESIGN.md#milestones).

## Why games stutter the first time

DX12 games hand the graphics driver a *pipeline* (shaders + render settings), and the driver
compiles it for your exact GPU. When that happens mid-game it freezes a frame for anywhere from a
few to a few hundred milliseconds. The driver keeps what it compiled in its shader cache, so the
second time you see that effect it's instant. That's why the stutter fades over time.

A game's own "compiling shaders" screen only covers the pipelines its developers listed. shaderprep
tries to fill the cache with everything else before you play.

## How it works

1. **Find the shaders.** Every DX12 shader is stored in the same standard container format,
   whatever engine made it. shaderprep looks for it
   - in the game's files on disk (works when shaders aren't packed into compressed archives), and
   - in the game's memory once it has loaded (full mode). The game unpacks its own archives,
     including encrypted ones, so nothing has to understand the archive format.
2. **Learn the game's style.** A shader alone isn't enough; the driver needs the whole pipeline.
   Each game reuses a small set of settings (root signatures, vertex layouts, render-target formats,
   blend modes). shaderprep watches the first pipelines the game creates (boot, main menu) and
   learns them.
3. **Infer and compile.** It combines every shader it found with the settings the game uses and
   compiles the results in the background, or before the game starts if you prefer. Wrong guesses
   cost almost nothing: D3D12 rejects them before the driver compiles anything.
4. **Record.** Every pipeline the game really creates is saved. Next time, or for another player you
   share the file with, those are compiled up front and match exactly.

## Two modes

| | **Full mode** | **Safe mode** |
|---|---|---|
| When | Games without anti-cheat (or if you accept the risk) | Picked automatically when anti-cheat is found |
| How | A `d3d12.dll` placed next to the game's .exe | A separate program runs before you start the game |
| Finds shaders in memory | Yes | No (disk scan, recordings, shared files only) |
| Records what the game uses | Yes | No |
| Touches the game process | Yes | **Never** |

**About anti-cheat** (e.g. Easy Anti-Cheat in Ace Combat 8 or Elden Ring): the compiled shaders land
in the driver's normal cache, which is harmless. What anti-cheat reacts to is a foreign DLL loaded
into the game. So shaderprep uses safe mode for these games by default. You can force full mode with
`--accept-anticheat-risk`, at your own risk: the game may refuse to start, or worse. Only do it for
offline single-player play.

Safe mode only helps if your graphics driver shares its shader cache between programs. Check it once
with `shaderprep-warm --cache-test` (see below).

## Usage

Download the Windows build (CI artifact `shaderprep-windows-x64`) or build it yourself (below).

```bat
:: See what shaderprep thinks of a game (engine, anti-cheat, mode)
shaderprep detect "C:\Games\SomeGame"

:: Set it up. Full mode: copies d3d12.dll next to the game exe. Safe mode: pre-compiles now.
shaderprep install "C:\Games\SomeGame"

:: Optional: hold the game at startup until everything known is compiled
shaderprep install "C:\Games\SomeGame" --block-at-boot

:: Remove the DLL again (your recordings are kept)
shaderprep uninstall "C:\Games\SomeGame"
```

After installing in full mode, start the game and **wait at the main menu for a minute or two**. That's
when shaderprep scans memory and compiles. Progress is written to
`<game exe folder>\shaderprep\log.txt`. Lines with `slow pipeline` are stutters that still got through.

Files in `<game exe folder>\shaderprep\`:

| File | What it is |
|---|---|
| `profile.toml` | Settings for this game (see [profiles/example.toml](profiles/example.toml)) |
| `recorded.psodb` | Pipelines the game really created. **Share this** with other players |
| `harvest.psodb` | Shaders found in memory |
| `disk.psodb` | Shaders found on disk |
| `import\*.psodb` | Drop recordings from other players here |
| `log.txt` | What happened |

### Sharing recordings

Pipelines are tied to a game build, not to your GPU, so a `recorded.psodb` from someone else helps you
too. Put it in `shaderprep\import\` (full mode) or pass it to `shaderprep-warm` (safe mode). Merge
several with:

```bat
shaderprep merge -o combined.psodb --as-imported a.psodb b.psodb
```

### Checking the driver cache (do this once)

```bat
shaderprep-warm --cache-test --seed 5     :: first run: slow (really compiles)
shaderprep-warm --cache-test --seed 5     :: second run: should be much faster
copy shaderprep-warm.exe other-name.exe
other-name --cache-test --seed 5          :: still fast => safe mode works with your driver
```

### Tools for measuring

```bat
shaderprep report recorded.psodb                                     :: what's in a file
shaderprep scan "C:\Games\SomeGame" -o disk.psodb                    :: disk scan only
shaderprep infer recorded.psodb harvest.psodb -o candidates.psodb    :: run inference offline
shaderprep analyze --truth later-session.psodb candidates.psodb      :: how much was covered
```

## Limitations (honest list)

- Shaders the game never loads into memory during the scan window (some engines stream them per
  level) are only covered by recordings, yours or shared ones.
- Graphics pipelines are guesses unless they were recorded. `shaderprep analyze` measures how good
  the guesses are for each game.
- Geometry, tessellation and mesh-shader pipelines are only replayed from recordings, not inferred.
- After a **driver update** the driver throws its cache away. Just launch the game again (or rerun
  safe mode) and shaderprep re-compiles everything it knows.
- After a **game patch**, recordings start fresh; the previous file is kept as `recorded-<build>.psodb`.
- Not a fix for "traversal" stutter (asset streaming), which has nothing to do with shaders.
- If the game already has a `d3d12.dll` next to its exe (another mod such as ReShade), install refuses.
  Use safe mode.
- 64-bit DX12 games only.

## Building

Requirements: CMake 3.20+, a C++20 compiler. DirectX-Headers is fetched automatically for Windows builds.

```sh
# Windows (Visual Studio 2022)
cmake -S . -B build -A x64
cmake --build build --config Release

# Linux: portable core + unit tests only
cmake -S . -B build && cmake --build build && ./build/shaderprep_tests

# Windows binaries from Linux (MinGW)
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-win
```

Code layout and design: [docs/DESIGN.md](docs/DESIGN.md).
