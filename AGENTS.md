# AGENTS.md — tracktion_engine

## Project

- C++20 audio engine library distributed as a **JUCE module**.
- Three modules: `tracktion_core`, `tracktion_engine`, `tracktion_graph` (registered via `juce_add_modules` with `ALIAS_NAMESPACE tracktion`).
- Depends on JUCE (git submodule at `modules/juce`, branch `develop`).
- Version in `VERSION.md` (currently 3.2.0).

## Build

- **CMake only.** No Makefiles, no Xcode/VS projects checked in.
- Use presets from `CMakePresets.json`:
  - Linux: `cmake --preset ninja-multi` → `cmake --build --preset ninja-<Config>`
  - macOS: `cmake --preset xcode` → `cmake --build --preset xcode-<Config>`
  - Windows: `cmake --preset windows` → `cmake --build --preset windows-<Config>`
- Build output goes to `cmake-build/<hostSystemName>/cmake-<presetName>/`.
- Targets: `DemoRunner`, `EngineInPluginDemo`, `TestRunner`, `Benchmarks`, `WavPlayer`.
- Build a single target for fast iteration: `cmake --build --preset <preset> --target <Target>`
- To reconfigure after CMakeLists.txt changes, add `--fresh`: `cmake --preset windows --fresh`
- Disable examples with `-DTE_ADD_EXAMPLES=OFF` when embedding as a submodule.

## Tests

- Tests run via the **TestRunner** console app (uses doctest).
- Build then run: `ctest --test-dir cmake-build/<platform>/cmake-<preset> -C <Config> -R TestRunner -V`
- **Rubber Band** time-stretch library is NOT included by default. Add it for full test coverage:
  ```
  git submodule add -f https://github.com/breakfastquay/rubberband.git modules/3rd_party/rubberband
  git submodule update --init
  ```
- Release builds compile with `-Werror` (clang/gcc) or `/WX` (MSVC).

## Scripts (require bash — use Git Bash on Windows)

- `tests/generate_examples` — generates IDE project files only (no build).
- `tests/build` — builds and runs TestRunner + Benchmarks, then builds DemoRunner + EngineInPluginDemo.
- Both scripts operate per-example under `examples/<name>/`, creating `examples/<name>/build/`.

## Code Style

- `.clang-format` at root: Allman braces, 4-space indent, no column limit, pointer left, C++20.
- Run `clang-format` on changed files before committing.

## JUCE Gotchas

- `juce::AudioTransportSource::setSource()` — if `readAheadBufferSize > 0`, you **must** also provide a valid `TimeSliceThread*`. Passing a non-zero buffer size with a nullptr thread causes a null dereference crash. Pass `0` for buffer size if you don't need read-ahead buffering.

## Module Source Layout

- `modules/tracktion_engine/` is split across multiple `.cpp` files (`tracktion_engine_model_1.cpp`, `tracktion_engine_playback.cpp`, etc.) for compilation-unit management.
- Subdirectories: `model/`, `playback/`, `plugins/`, `midi/`, `audio_files/`, `timestretch/`, `control_surfaces/`, `selection/`, `project/`, `utilities/`.

## CI (`.github/workflows/`)

- `build.yaml`: matrix of Debug/Release × Linux/macOS/Windows × 4 targets. Also ASan, TSan, and static analysis on macOS.
- `generate_coverage.yaml`: Linux-only coverage via lcov, uploaded to Codecov.
- `juce_compat.yaml`: verifies compatibility with JUCE main branch.
- Checkout must use `submodules: true`.

## Contribution

- **No direct GitHub PRs accepted** (copyright restrictions). Contact Tracktion via JUCE forum to contribute.
- Issues/bug reports: https://forum.juce.com/c/tracktion-engine

## Key Licenses

- GPL v3 or commercial (Tracktion Engine)
- JUCE is a separate dependency with its own license
- Bundled 3rd-party: rpmalloc (public domain), choc (ISC), doctest (MIT), farbot (MIT), magic_enum (MIT), others — see README.md
