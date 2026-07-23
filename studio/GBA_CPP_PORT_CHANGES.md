# GBA C++ Port and Studio Integration — Changes

Date: 2026-07-22

## Implemented

### `extra/gba++`

- Restored/imported the attributed C++ GBA platform core at upstream commit
  `13cae89f9dba719454c283e330bf9e131af68c8c`.
- Preserved its ARM7TDMI ARM/THUMB decoder, interpreter oracle/self-healing
  bridge, static C++ generator, GBA bus/PPU/APU/DMA/timers/IRQ/save/RTC model,
  SDL host, TCP diagnostics, tests, tools, documentation, and licenses.
- Vendored the pinned toml++ v3.4.0 amalgamated header (commit `30172438`) so
  `gba_recompile` and generated source repositories configure offline.
- Added parent-provided SDL2 support for deterministic Studio native and cross
  builds.
- Fixed the codegen-test output-directory/executable collision on macOS.
- Fixed the `armv4t::Bus` forward declaration (`class` vs `struct`) that warned
  about possible Microsoft ABI linkage failure.

### Studio model and UI

- Added `PlayStation` / `Game Boy Advance` system selection and request JSON
  serialization.
- Added single-ROM and recursive `.gba` batch discovery with header metadata,
  title/game-code extraction, save-type signature detection, and warnings for
  unusual-but-size-valid images.
- Persisted PSX and GBA input/BIOS/batch paths independently.
- GBA mode relabels the inputs and hides Ghidra, PSX BIOS branding, PSX boot,
  controller-negotiation, and macOS GIP-only controls.
- Existing title/icon/platform/All/ZIP/source/build/signing/local/CI/queue/
  overwrite/progress/cancellation controls are reused.

### Studio GBA pipeline

- Validates ROM size/header metadata and a 16 KiB BIOS, then records SHA-1,
  SHA-256, and CRC32 proof.
- Builds `gba_recompile`, emits deterministic 16-shard C++, hashes every
  generated file, and records the honest AOT/self-heal coverage policy.
- Produces an initialized Git source repository containing the isolated C++
  core, generated code, selected package inputs, licenses, build rules, and
  proof artifacts.
- Generated apps resolve packaged assets from their platform package location,
  use per-user writable save/cache directories, and pin the selected ROM/BIOS
  hashes.
- Native and CI builds expose the existing `psx-runtime` target/package contract
  so the current Studio/Steganos flow can build either system without a CI
  protocol fork.
- macOS output is `<Title>.app` with `Contents/MacOS`, `Contents/Resources`,
  `Contents/Frameworks`, a valid ICNS icon, embedded SDL2/SDL3 when needed,
  proof archive, licenses, ROM, BIOS, and `game.toml`.
- Windows/Linux generated projects keep the existing root executable/resources/
  `game.manifest.json` package shape and use the current pinned SDL/toolchain
  strategy.
- Source and build delivery use overwrite-safe staging; ZIP roots match the PSX
  contract.

### Tool repair

- `iconutil` on the current macOS host rejected otherwise standard iconsets.
  Studio now falls back to a deterministic PNG-backed ICNS writer and verifies
  the resulting `icns` container, rather than skipping app-icon verification.

## Verification

- Rust behavioral reference: `cargo test --workspace --release` — **168 tests
  passed**.
- C++ port: configure/build plus `ctest --output-on-failure` — **16/16 tests
  passed**.
- Studio: `PSXRecompStudioTests` and BIOS branding tests — **2/2 passed**.
- Private local ROM translation smoke: 4 MiB cartridge, **3,719 functions**,
  **16 generated shards**, approximately one second for translation.
- Studio source-export smoke completed and produced a committed portable source
  repository.
- Studio native macOS build smoke completed. Verified:
  - valid x86-64 Mach-O;
  - valid PNG-backed ICNS;
  - `@rpath/libSDL2-2.0.0.dylib` with bundled SDL2 and SDL3 compatibility
    runtime;
  - `codesign --verify --deep --strict` passes;
  - expected ROM/BIOS/config/licenses/proof resource layout.
- `git diff -- runtime/` is empty for this task; pre-existing runtime working
  tree edits were not touched.

## Parity boundary

This is the existing attributed C++ port baseline, not a mechanical file-for-file
translation of every Rust crate. Studio replaces the Rust launcher/packager
front end. The C++ core covers the load-bearing recompiler/runtime path and adds
its own TCP/self-healing diagnostics. Rust-only product surfaces that are not in
this C++ baseline include the egui launcher, SQLite `gamedb` reader, standalone
`gba-pack` CLI vocabulary, wgpu/Wayland presentation backend, and the GAX/RDRV
shadow engines. Those differences are preserved and documented rather than
filled with guessed implementations.
