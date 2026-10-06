# Design

## Goal

Remove first-playthrough shader stutter in DX12 games with a tool that works for any engine, not one
written for each game. Engine modules and per-game profiles are optional extras on top of a generic core.

## Facts the design rests on

- **A hook only sees what the game requests.** The other combinations have to come from harvesting
  shaders plus inference.
- **Engine-agnostic discovery.** Every D3D12 shader (DXBC or DXIL) and every serialized root
  signature is a `DXBC` container with a recognisable header. The game decompresses and decrypts
  its own archives, so scanning its memory finds shaders without understanding any archive format.
  Loose files on disk work the same way.
- **The driver cache matches only on the whole pipeline description.**
  - Compute pipeline = shader + root signature. That's exact when the root signature is embedded
    (`RTS0` part) or learned from the game.
  - Graphics pipelines also need input layout, render-target/depth formats, blend, rasterizer and
    depth state, etc. Games reuse a small set of these, learned from the PSOs they create.
- **Wrong guesses are cheap.** The runtime validates root signature against shaders and input
  layout against the vertex shader before calling the driver, so mismatches fail with `E_INVALIDARG`
  without a compile.
- **Driver caches may be per executable.** That's why full mode compiles inside the game process.
  Safe mode depends on milestone M0(b).
- **Anti-cheat reacts to the method** (DLL injection, hooks, memory reads), not the output, which
  is the driver's normal cache.

## Architecture

```
src/core/      .psodb format: content-addressed blobs (xxHash3), portable PsoDesc, game identity
src/dxbc/      DXBC/DXIL container parser: stage, signatures, embedded root signature
src/harvest/   container scanner for files and memory (streaming, handles chunk boundaries)
src/infer/     style model + candidate generation
src/analyze/   coverage report (exact hits, why each miss happened)
src/profile/   per-game TOML profile, anti-cheat detection, engine/exe guess
src/cli/       shaderprep.exe
src/win/       D3D12 bridge (capture/replay), warm thread pool, memory scan, log
src/hook/      proxy d3d12.dll (exports forward to System32), vtable hooks, runtime
src/helper/    shaderprep-warm.exe: safe-mode compiler + driver-cache experiment
tests/         unit tests (portable), tests/win/hook_smoke.cpp end-to-end proxy test
```

### PsoDesc

Every way of creating a pipeline (`CreateGraphicsPipelineState`, `CreateComputePipelineState`,
`CreatePipelineState` streams, `ID3D12PipelineLibrary::Load*`) is converted into a sorted list of
subobjects keyed by `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE`:

- shaders and root signatures become blob hashes,
- input layout / stream output / view instancing become owned vectors,
- all other state is kept as raw x64 struct bytes with padding zeroed.

`CACHED_PSO` is dropped. The hash ignores which API was used. Replay uses the API the game used.

### Inference

1. Compute shader with an embedded root signature → exact candidate, reusing the game's root
   signature object with the same RTS0 payload when one was recorded.
2. Pixel shader → compatible vertex shaders (VS outputs cover the PS inputs; registers too for
   legacy DXBC). VS the game was seen using come first. Each pair gets the styles seen with that
   VS, then the globally most frequent styles, filtered by input-layout coverage and render-target
   count.
3. Unused vertex shader → depth-only styles (pipelines recorded without a PS).
4. Compute shader without an embedded root signature → the game's most used compute styles.

Limits are set in `[infer]` in the profile.

### Runtime (full mode)

- On the first `D3D12CreateDevice` (or `ID3D12DeviceFactory::CreateDevice`): load the profile and
  the databases (disk, recorded, harvest, imports), infer candidates, hook the device vtable, and
  start one Warmer for the hardware adapter.
- Recording: every successful game PSO creation is timed and captured. Root signature objects map
  to their blobs through the `CreateRootSignature` hook. Saved every 10 s.
- Harvest: at the profile's `scan_delays_s`, scan committed private/mapped memory (skipping images,
  guard and write-combined pages; reads go through `ReadProcessMemory` so a page freed mid-scan
  can't crash the game). Then re-learn the style including this session's recordings, infer, and
  queue new candidates.
- Warm threads run below normal priority, except while holding the game at boot.

## Milestones

- **M0 feasibility (Windows, needs real games):**
  - (a) proxy loads in an Unreal 5, an RE Engine and a FromSoftware game (offline, no anti-cheat)
  - (b) `shaderprep-warm --cache-test` on NVIDIA and AMD: is the cache shared across executables?
  - (c) how many shaders the main-menu memory scan finds (`log.txt`)
- **M1** core, dxbc parser, unit tests: done
- **M2** RECORD + miss timing: done
- **M3** WARM in process, block-at-boot, safe-mode helper: done
- **M4** disk + memory harvest: done
- **M5** style model + candidates: done (first heuristics)
- **M6** analyze + profiles: done. Learned data is kept in `recorded.psodb` (styles are re-learned
  at each start) rather than written into the TOML.
- **M7** Unreal bundled PSO cache (`.upipelinecache`) module: not started. Further engine modules
  only where M6 data shows big gaps.
- **M8** sharing: file-based import/export/merge done. Online repository: out of scope for now.

## Verification

- `shaderprep_tests`: dxbc parsing, psodb round trip / merge / corruption, harvest over noise,
  chunk boundaries and directory scans, inference (compute exact / guessed, VS–PS pairing, style
  filtering), coverage analysis, profiles, anti-cheat detection. Runs on Linux and Windows.
- `hook_smoke` (CI on WARP; also run under Wine + vkd3d + lavapipe during development):
  1. record: classic graphics, stream and compute pipelines are recorded
  2. check: a pixel shader the "game" compiled but never used is found by the memory scan, and a
     candidate is inferred for it
  3. warm: on the next start, all recorded and inferred pipelines replay (0 failed)
- Real games (manual):
  - first launch in record-only mode → slow-pipeline count A
  - clear the driver cache, fresh first launch with harvest + warm → count B
  - also PresentMon frame times
  - `shaderprep analyze` gives the exact-hit rate
