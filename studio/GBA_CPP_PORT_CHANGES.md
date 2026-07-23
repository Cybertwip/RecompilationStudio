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

- Validates ROM size/header metadata and hashes all inputs. A canonical 16 KiB
  BIOS uses LLE; an absent or noncanonical image uses standalone BIOS HLE with
  a deterministic zero placeholder that is never executed.
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
- Freeze/render repairs verified against the Rust reference:
  - standalone BIOS HLE now implements reset/wait calls and IRQ dispatch;
  - forced-stop interpreter bridges no longer skip RAM continuations or static
    callees;
  - bitmap BG modes 3/4/5 render through BG2;
  - bitmap OBJ tile indices below 512 are rejected and tile 512 maps to
    `0x14000`;
  - a no-HALT watchdog preserves state and switches a stalled standalone-HLE
    session to the reference interpreter instead of leaving the window frozen.
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
  - expected ROM/BIOS/config/licenses/proof resource layout;
  - language selection and warning screen match the Rust reference visually;
  - 4,000 frames beyond the former intro freeze continue successfully;
  - standalone-HLE force-interpreter IRQs now use an asynchronous prologue/
    `0x138` epilogue state machine, eliminating the later `0x02000014`
    undefined-instruction crash;
  - mutable ROM/BIOS sidecars are stored in per-user data, so launching an app
    no longer invalidates its code signature;
  - canonical hardware audio samples/FIFO state remain live.
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


## Runtime fallback disclosure

Generic ROM discovery still leaves interior dispatch gaps. The runtime reports
those gaps, bridges them through the reference ARM7TDMI interpreter, and in
standalone-HLE mode switches the whole session to that interpreter if generated
AOT code spends a sustained second without HALT progress. This is deliberate,
state-preserving, and loudly reported as `DEGRADED`; it prevents the later intro
freeze but means such a session is not honestly “fully static.” A canonical-BIOS
build remains on the normal AOT/LLE path unless it encounters its own reported
coverage gap.

The generic Studio profile disables the MP2K enhanced-audio shadow by default.
FFVI’s driver variant failed the verifier and already reverted to canonical
hardware audio; disabling the shadow removes the misleading degradation message
and avoids extra audio work while retaining the verified FIFO/canonical output.
