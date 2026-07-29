# GBA C++ Port and Studio Integration Plan

Date: 2026-07-22

> **SUPERSEDED 2026-07-28.** Studio's GBA runtime is `extra/gbarecomp`, driven by
> Ghidra-seeded static recompilation through `gba_recompile`. `extra/gba-rust` is
> no longer a behavioral reference, `extra/gba++` was never adopted, and
> constraint 4 below ("Studio GBA builds do not invoke Ghidra") is reversed: GBA
> function discovery is now Ghidra-seeded exactly like the PlayStation flow. See
> `studio/README.md` → "Game Boy Advance flow". This file is kept for the record.

## Objective

Port the self-contained `extra/gba-rust` GBA recompilation stack into
`extra/gba++`, then make PSXRecomp Studio build GBA titles through the same
platform/export flow used for PlayStation titles. The root `runtime/` tree is
read-only for this work.

## Behavioral source and C++ baseline

- `extra/gba-rust` is the behavioral/product reference supplied for this task.
- The C++ baseline is the existing `mstan/gbarecomp` port at commit
  `13cae89f9dba719454c283e330bf9e131af68c8c` (2026-07-20), which this
  repository already credits in `THIRD_PARTY_ATTRIBUTION.md`.
- The imported baseline remains isolated under `extra/gba++`; its license and
  attribution files travel with it.
- Differences between the Rust reference and the C++ baseline are documented,
  not silently filled with guessed behavior.

## Constraints

1. Do not modify any file under the repository-root `runtime/` directory.
2. Do not introduce a dependency from the GBA path on the PSX runtime.
3. Generated ROM-derived files stay build/workspace artifacts and are never
   committed.
4. Studio GBA builds do not invoke Ghidra; the GBA toolchain is self-contained.
5. A delivered GBA package uses the same outer contract as a Studio PSX
   package:
   - macOS: `<Title>.app` at archive root, executable in
     `Contents/MacOS`, resources in `Contents/Resources`;
   - Windows/Linux: executable and resources at package/archive root;
   - `game.manifest.json` at archive root;
   - optional unpacked delivery, overwrite-safe staging, proof archive, and
     platform-specific icon handling.
6. Studio follows its existing personal-export contract: the selected ROM and
   BIOS are copied into the generated source workspace and final app resources,
   hash-pinned in `game.toml`, and never added to this repository. This differs
   deliberately from `gba-pack`'s redistributable/no-ROM package policy so GBA
   apps match the current PSX Studio app structure.

## Implementation phases

### 1. Import and normalize `extra/gba++`

- Import the tracked C++ port source, tests, tools, docs, licenses, and CMake.
- Add an embedding mode so Studio can build only the libraries/tools it needs,
  without the upstream oracle, heavyweight optional targets, or network fetches.
- Vendor/pin the small TOML parser dependency needed by `gba_recompile` so
  Studio source exports are offline-rebuildable.

### 2. Add a generic generated-game target

- Add a small game executable entry point in `extra/gba++` that calls the C++
  runtime with Studio-provided title/hash defaults.
- Add a reusable CMake function that consumes generated shards plus the GBA
  runtime and produces a platform-native GUI executable/app target.
- Resolve the packaged ROM/BIOS relative to the executable, enforce their
  hashes, and redirect saves/caches to a per-user writable data directory.

### 3. Extend Studio's request model and UI

- Add an explicit system selector (`PlayStation` / `Game Boy Advance`).
- Reuse the existing title, icon, target platform, ZIP, output, signing, queue,
  CI/local, progress, cancellation, and delivery controls.
- Switch the primary input label/filter between PSX disc and GBA ROM.
- Hide or disable PSX-only branding/Ghidra controls for GBA requests.
- Persist the selected system and GBA input independently.

### 4. Add the GBA pipeline

- Validate the GBA ROM header/size and select canonical LLE or standalone HLE from the optional 16 KiB BIOS.
- Hash both inputs and create a deterministic temporary workspace.
- Build `gba_recompile`, generate sharded C++, and generate a standalone CMake
  project that links `extra/gba++` without touching root `runtime/`.
- Build/package for the selected platform using Studio's existing toolchain and
  package conventions.
- Create a proof archive containing input hashes, tool/source revision,
  generation manifest, and delivered executable hashes.

### 5. Verification

- Build and run the imported C++ unit tests.
- Add Studio unit tests for system serialization, output naming, manifests,
  validation, generated CMake, and package layout.
- Exercise a source export without proprietary inputs using a synthetic valid
  GBA fixture.
- Build a representative native macOS app from locally available private test
  inputs, inspect its bundle/resources/dependencies, and verify its signature.
- Confirm `git diff -- runtime/` contains no task changes.

## Deliverables

- `extra/gba++/`: C++ GBA recompiler/runtime/tooling snapshot plus integration
  helpers.
- Studio source/model/pipeline/tests for GBA selection and export.
- `studio/GBA_CPP_PORT_CHANGES.md`: implemented changes, verification commands,
  results, and known parity differences from `extra/gba-rust`.
