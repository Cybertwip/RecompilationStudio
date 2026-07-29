# PSXRecomp Studio

PSXRecomp Studio is the Qt front end for producing self-contained macOS,
Windows, Linux, or **Virtua ARM** PlayStation apps and Game Boy Advance packages
without creating a permanent per-title repository. A Windows Studio build exports
Windows packages and a Linux Studio build exports Linux packages. macOS can
select macOS, Windows, Linux, Virtua ARM, or **All**; All queues the three
desktop targets into one output directory.

It uses Qt 6 Widgets, Oclero Qlementine, Qlementine Icons,
QtAppInstanceManager, and QuaZip. Work happens in a temporary directory; the
selected output directory receives only the final platform package.

## Game Boy Advance flow

Choose **Game Boy Advance** in the System selector, then provide one `.gba`
cartridge image and the canonical 16 KiB `gba_bios.bin`. Studio statically
recompiles both with `extra/gbarecomp` — the same shape as the PlayStation flow.
Batch mode scans recursively for `.gba` files.

Function discovery is seeded by a headless Ghidra analysis (`ARM:LE:32:v4t`,
raw image based at `0x08000000`, `TMode` deciding ARM vs THUMB per function).

**Ghidra is a seed source here, not a coverage oracle.** A cartridge image has
no section table, so Ghidra disassembles compressed art and sample data exactly
as willingly as it disassembles code, and it cannot tell which is which. On
*Final Fantasy VI Advance* its stock analysis reports 120,079 functions whose
bodies cover 99.6% of the 16 MiB ROM, 27,929 of them one byte long. Studio
disables the byte-pattern function finders (`Function Start Search`, `… After
Code`, `… After Data`) and `Non-Returning Functions - Discovered`, which brings
body coverage to 50.9% — still far past the ~2.8% that is really code, because
one word of sample data decoded as a branch generates references that
manufacture more functions. Requiring the dispatch table to contain every
Ghidra function would therefore mean translating megabytes of graphics.

**Ghidra's reference classes are not trusted either**, because they are derived
from that same decoding. A THUMB `BL` is two halfwords; where Ghidra decodes a
second halfword with no first halfword in front of it, it models the result as a
call through `LR` and propagates a destination into it — manufacturing a
`COMPUTED_CALL` out of data. On FFVI that produces **113,516 `COMPUTED_CALL`
references against 22 `COMPUTED_JUMP`**, its only real switch-table recovery.
Seeding on reference class alone admits those artifacts, and because each
admitted seed extends the translated extent, the next pass admits more of them:
a self-feeding cascade that on FFVI reached 834,559 dispatch rows and 1.5 GB of
generated C++ by pass 11.

So each pass asks **`gba_recompile`, not Ghidra, what the cited source address
contains**. The shards carry both descriptions of the translation: a
`/* 0xSTART mode=… end=0xEND … */` header above every lowered function, and one
`/* ADDR  addr T mnemonic … */` comment per lowered instruction. Both come from
recursive descent along real control flow out of the entry point, so neither
walks into a texture. Being inside a translated extent is necessary but not
sufficient — a span of real code can still have a Ghidra reference hanging off a
byte offset that is not an instruction boundary at all. Three rules admit a
candidate:

- **direct-branch** — the recompiler decoded a `B`/`Bcc` or a *complete* `BL`
  pair at the cited address, and its resolved destination is this entry. The
  site's instruction set must match: ARMv4T has no `BLX`, so a direct branch
  cannot change instruction set, and a mismatch means one side decoded data.
- **register-indirect** — the recompiler decoded a `BX` or other `PC` write
  there, so control provably leaves and it could not resolve where. This is the
  one thing Ghidra can genuinely add. No mode constraint applies, because a `BX`
  carries the destination instruction set in bit 0.
- **pointer-word** — a data reference from a word inside a translated extent
  whose stored value is exactly `entry | 1` for THUMB or `entry` for ARM, since
  bit 0 of a `BX` target selects the instruction set. An exact test, not a
  heuristic.

Rejected as evidence: a cited address that is not an instruction boundary; an
orphan `bl.lo`, because a branch whose target is unresolved says nothing about
where control goes; and a direct branch resolving somewhere other than the
candidate.

Admitted entries are appended to the symbol TSV and the recompiler runs again;
more translated code exposes more evidence, so the passes iterate to a fixed
point. The build converges when a pass admits nothing new — not when Ghidra's
list is exhausted — and fails only if the fixed point is not reached within
eight passes. `game.toml` is never auto-written; the recompiler is seeded
through the TSV instead. `ghidra_coverage_proof.json` records every pass with
per-rule counts and sampled admissions and rejections.

On FFVI this converges on the **first** pass: every candidate Ghidra offers is
either already translated or backed only by references manufactured from asset
data. That is the honest result for this title, not a failure — the recompiler's
own recursive descent and jump-table walking had already found 16,129 functions
covering 464,308 bytes, concentrated in the first two MiB where the code
actually lives, plus the IWRAM-copied routines higher up. All seven `BX`-sourced
candidates that survive decoding were already covered: the recompiler resolves
its own interworking veneers. The rules stay in place because they are general —
on a title whose tables defeat the recompiler's `auto_jt` bounding, the
register-indirect rule is what supplies the destination.

An address both tools translated in *different* instruction sets is recorded in
that proof, not fatal: Ghidra loses these on a raw cartridge (it has been
observed calling four bytes in the middle of a THUMB run an ARM function),
while the recompiler reached the address by following real control flow.
Whatever remains unreached at the fixed point is handled honestly at runtime —
a dispatch miss bridges through gbarecomp's reference interpreter, is counted
in its coverage banner, and is emitted as a reviewable `game.toml` proposal
fragment, never silently absorbed.

The BIOS is recompiled and dispatched, never stubbed, HLE'd, or skipped: every
title's first frames are real BIOS frames. There is no boot-skip option.

CMake remains the CI entrypoint (`gba-runtime`). The exported project compiles
the checked-in `generated/recompiled_*.cpp` shards and links the gbarecomp
runtime libraries; it never needs the ROM, the network, or the recompiler at
build time, and it refuses to configure if `generated/` is empty. Packages
include the selected ROM and BIOS as resources, hash-verified at startup against
the identities recorded in `game.toml`.

The GBA runtime provides automatic keyboard/gamepad input, the shared static
macOS Xbox/PDP GIP backend, keyboard rebinding, an in-game settings menu, and
persistent audio/video/controller settings.

**Virtua ARM is not available for GBA yet.** The bundled Virtua SDL
compatibility surface (`extra/virtua/sdl`) does not implement the renderer
viewport/fill/line calls, blend modes, audio device status, controller sensor,
or touch entry points that the gbarecomp host layer uses. The platform entry is
greyed out for GBA rather than producing a package that cannot run.

## Required inputs

- One CUE sheet and every BIN file it references, or one standalone raw BIN.
  Multi-track CUE sheets are supported. **Batch** mode instead accepts a
  directory, scans it recursively, groups CUE-owned tracks, and creates an
  editable game list.
- An exact North American `SCPH1001.BIN` image:
  - size: `524288` bytes
  - SHA-256: `71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3`
- An optional PNG, SVG, or ICNS app icon. The built-in PSXRecomp icon is used
  when no icon is selected. Batch mode has an independent optional icon picker
  for every game.
- Optional BIOS branding images: one initial splash and one handoff image.
- The desired runtime window title. This also becomes the app name.
- For optional macOS signing, a password-protected PKCS#12 identity (`.pfx` or
  `.p12`) and its password. macOS, Windows, and Linux exports can all be
  delivered unsigned; Windows and Linux exports are currently always unsigned.
- Ghidra 11.3.2 with Java 21.

The certificate password is never persisted in settings. During a signed build,
Studio writes it to an owner-only temporary file, passes the original PFX
directly to `rcodesign`, then deletes the password file. The identity is never
imported into the login or system Keychain, so signing does not trigger repeated
Keychain authorization dialogs. `rcodesign` recursively signs the bundle in one
pass; Apple's `codesign` independently verifies the result.

## Batch and platform behavior

Enable **Batch**, choose the game directory, then edit the detected names and
pick optional icons from the generated list. Icon choices are keyed to each
disc's stable source identity and restored on later Studio runs. One selected
BIOS and the common runtime/branding settings apply to the whole queue. A
directory containing both a CUE and its referenced BIN tracks produces one game
entry, not one entry per track; unowned standalone BIN images become their own
entries.

On Windows and Linux, Batch exports every listed game for the native host
platform. On macOS, an individual platform does the same for that target, while
**All** exports each listed game for macOS, Windows, and Linux.

**Export as zip** is enabled by default and creates one archive per game and
platform. A macOS archive contains `Game Name.app` at ZIP root. Windows and
Linux archives contain the package contents directly at ZIP root, so the native
executable is not hidden inside an extra package-directory layer. Disable the
option to retain the unpacked `.app`, `-Windows`, and `-Linux` outputs.
Every archive also contains a root `game.manifest.json` with `name` set to the
edited game title and `executable` set to the root `.app`, `.exe`, Linux binary,
or `.virtua` entry point. Virtua ARM packages use the `Virtua ARM` platform
label.

## Host tools

Every export requires CMake; non-Windows hosts also require Ninja. PlayStation
and Game Boy Advance exports additionally require Ghidra 11.3.2 and OpenJDK 21;
PlayStation exports also require Python 3.
Virtua ARM builds additionally require Go, a PowerEngine source checkout, and
its LLVM/compiler bundle. Studio passes `POWERENGINE_ROOT` and
`VIRTUA_LLVM_ROOT`; Dash, MVII POSIX headers, the Virtua packager, llvm-libc,
libc++, and compiler-rt are consumed directly from those roots rather than
copied under `extra/virtua`. On macOS, Studio detects both registered JDK bundles
and keg-only Homebrew `openjdk@21` installations, then passes that exact
`JAVA_HOME` to Ghidra.
macOS exports additionally require:

- Xcode command-line tools (`iconutil`, `otool`, and `install_name_tool`)
- SDL2 development files discoverable through `pkg-config`
- libusb 1.0 development files, including `libusb-1.0.a`, for the default wired
  Xbox/PDP controller export (`brew install libusb`)
- When Homebrew provides SDL2 through `sdl2-compat`, the matching SDL3 runtime
  must be installed; Studio bundles and signs both libraries.
- For optional PFX signing, `rcodesign` 0.26.0 or newer from the
  `apple-codesign` project and
  Apple's `codesign`. Studio checks `PSXRECOMP_RCODESIGN`, its bundled tools
  directory, `~/.cargo/bin`, and `PATH`, in that order. One installation method
  is `cargo install apple-codesign`.

Native Windows exports require Visual Studio 2022 with the MSVC x64 tools.
Windows exports from macOS instead require the Homebrew MinGW-w64 tools named
`x86_64-w64-mingw32-gcc`, `x86_64-w64-mingw32-g++`,
`x86_64-w64-mingw32-windres`, `x86_64-w64-mingw32-ar`,
`x86_64-w64-mingw32-ranlib`, `x86_64-w64-mingw32-strip`, and
`x86_64-w64-mingw32-objdump`. Studio writes an explicit CMake toolchain file
for these programs. It downloads SDL2's official, checksum-pinned MinGW
development archive and statically links SDL2 plus the GCC/C++ runtimes so the
delivered executable has no non-system DLL dependency.

Native Linux exports use the host `gcc`, `g++`, `ar`, `ranlib`, `strip`, and
`readelf`. Linux exports from macOS instead require the tools named
`x86_64-unknown-linux-gnu-gcc`, `x86_64-unknown-linux-gnu-g++`,
`x86_64-unknown-linux-gnu-ar`, `x86_64-unknown-linux-gnu-ranlib`,
`x86_64-unknown-linux-gnu-strip`, and
`x86_64-unknown-linux-gnu-readelf`. Studio writes an explicit CMake toolchain
file for these programs. It uses checksum-pinned SDL2 2.32.10 headers and a
checksum-pinned manylinux x86-64 SDL2 runtime, statically links the GCC and C++
runtimes, and bundles SDL2 beside the executable with an `$ORIGIN` runtime
path. Linux input uses SDL's kernel evdev path rather than direct HIDRAW access,
loads a checksum-pinned SDL GameController mapping database, and routes Player
1's `auto` selection to the keyboard whenever no physical controller is live.

**Controller type is negotiated per game at runtime.** Every Studio export
starts with a real D-Pad. If the title repeatedly requests DualShock
configuration without accepting a normal D-Pad poll, that port promotes to
Hybrid. The generated `game.toml` records `default_mode = "auto"` with
`lock_mode = true`, so Batch exports do not need a controller choice per title.
The resulting package records its actual minimum glibc requirement from the
versioned symbols observed in the delivered executable and bundled SDL2 runtime;
it does not declare or enforce a fixed glibc baseline. The package is also
verified to have no non-system shared-library dependency outside bundled SDL2.

Qt is bundled into the distributed Studio app. Qt is not linked into generated
game apps; those use the PSXRecomp SDL runtime.

## PlayStation pipeline

The Game Boy Advance pipeline is described under "Game Boy Advance flow" above.

1. Parse the CUE and validate every referenced BIN, or validate the selected
   standalone BIN. Batch mode performs this discovery once and queues the
   resulting game entries.
2. Read `SYSTEM.CNF`, extract the boot PS-X EXE, and validate its header.
3. Validate the exact SCPH1001 BIOS revision.
4. Import the stripped game image into a temporary Ghidra project as
   `MIPS:LE:32:default` at the EXE load address.
5. Prove the entry function and first instruction against the raw EXE bytes.
   This uses Ghidra headless analysis and does not disturb an already-open
   Ghidra project.
6. Build `psxrecomp-game` and `psxrecomp-bios` in a shared tool cache.
7. Generate game code, compare the generated entry manifest with Ghidra, add
   only uncovered Ghidra-proven entries, and repeat until coverage converges.
8. Optionally patch the BIOS TIM branding assets, remove the stock SCE/PS
   geometry routines, and suppress BIOS-shell SPU key-on writes; then generate
   native C from that patched BIOS. The original BIOS file remains unchanged.
9. Run the generated-code, dispatch, range, data, and runtime-installed-target
   audits. Any unresolved disagreement stops the build.
10. Compile a Release macOS application bundle, x86-64 Windows GUI executable,
    or x86-64 Linux ELF executable. The macOS path verifies the selected wired
    Xbox/PDP GIP backend and bundles SDL2/SDL3 as needed. The Windows path
    selects the installed MinGW-w64 toolchain, embeds a Windows icon, statically
    links SDL2, and rejects non-system runtime DLL imports. The Linux path
    selects the installed `x86_64-unknown-linux-gnu` toolchain, bundles SDL2,
    verifies the controller database, rejects unexpected ELF dependencies, and
    records the delivered binaries' observed glibc requirement without imposing
    a fixed compatibility baseline.
11. For macOS, either leave the app unsigned and hash-verify its staged payload,
    or pass the selected PFX directly to `rcodesign`. The signed path recursively
    seals the bundle once with Hardened Runtime, a secure timestamp, and the
    narrow `disable-library-validation` entitlement required by
    non-Apple/self-issued code-signing identities. It never imports the identity
    into a Keychain. Run strict Apple `codesign` verification before and after
    delivery.
12. By default, stream the verified package into a ZIP64 archive, preserving
    executable permissions, empty directories, hidden files, and symbolic
    links. Reopen every entry, verify its CRC and SHA-256 against the source,
    then atomically publish the archive. If ZIP export is disabled, copy the
    verified `.app`, `-Windows`, or `-Linux` package into the selected output
    directory and verify the delivered copy again.

A direct JAL into bytes classified as static data is emitted only as a
runtime-installed target. The bytes remain absent from native C and the runtime
requires dirty-RAM evidence before its interpreter can execute them. Canonical
A0/B0/C0 BIOS call thunks are recognized separately and remain native code.

## Generated app layout

Default macOS output (`Game Name-macOS.zip`) has one root package:

```text
game.manifest.json
Game Name.app/
  Contents/
    MacOS/Game Name
    Frameworks/libSDL2-2.0.0.dylib
    Resources/
      AppIcon.icns
      game.toml
      PSXRecomp-Proof.zip
      bios/SCPH1001.BIN
      game/<serial boot EXE>
      disc/<CUE and referenced BIN files, or standalone BIN>
```

Default Windows output (`Game Name-Windows.zip`) places these entries directly
at ZIP root:

```text
game.manifest.json
Game Name.exe
game.toml
PSXRecomp-Proof.zip
bios/SCPH1001.BIN
game/<serial boot EXE>
disc/<CUE and referenced BIN files, or standalone BIN>
seeds/ghidra_funcs.txt
```

Default Linux output (`Game Name-Linux.zip`) likewise places package contents
directly at ZIP root:

```text
game.manifest.json
Game Name
libSDL2-2.0.so.0
gamecontrollerdb.txt
AppIcon.png
game.toml
PSXRecomp-Proof.zip
licenses/SDL2.txt
licenses/pysdl2-dll.txt
licenses/SDL_GameControllerDB.txt
bios/SCPH1001.BIN
game/<serial boot EXE>
disc/<CUE and referenced BIN files, or standalone BIN>
seeds/ghidra_funcs.txt
```

### macOS wired Xbox/PDP controller export

**Enable wired Xbox/PDP controllers on macOS** is enabled by default. Studio
requires the static libusb archive and rejects an export unless the finished
PSX or GBA executable contains the native GIP symbols and has no dynamic libusb
dependency. The same `lib/recomp_gamepad` implementation is linked by CMake and
by the Rust `native-gamepad` FFI crate. Player 1
uses `p1_device = "auto"`; the runtime loads that packaged default before
per-install `settings.toml` overrides, so SDL controllers remain preferred and
the direct USB backend activates when SDL cannot expose the wired device. USB
OUT packets are copied to writable storage before entering libusb, which keeps
the backend compatible with the Hardened Runtime signature used by Studio.

The proof archive contains `macos_gip_controller.json` and records whether the
backend was requested, compiled, statically linked, and symbol-verified. The
physical hardened-runtime regression is reproduced by
`tools/test_macos_gip_hardened`.

Writable state is not placed in the signed app bundle. Settings, input maps,
memory cards, and overlay caches live under:

```text
~/Library/Application Support/PSXRecomp/<game serial>/
```

## Build Studio

```sh
cmake -S studio -B studio/build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build studio/build --target PSXRecompStudio PSXRecompStudioTests
ctest --test-dir studio/build --output-on-failure
cmake --install studio/build \
  --prefix /absolute/output/path --component Studio
```

The local self-contained build produced by this checkout is:

```text
dist/PSXRecomp Studio.app
```

## Failure behavior

The pipeline fails closed. It does not generate HLE shims, static-code
interpreter fallbacks, or placeholder functions. On failure, the temporary
workspace path is shown so its manifests and generated evidence can be
inspected. No persistent text log is written by Studio.

Studio signs but does not notarize generated apps. Notarization requires a
separate Apple notary credential and submission step.

## Optional BIOS branding patch

Enable **Patch BIOS branding for this app** to build against a derived BIOS:

- the selected initial image becomes a full-frame 640x480 textured quad;
- the COMPUTER ENTERTAINMENT/TM assets and both SCE diamond paths are removed;
- the frontend stays black through pre-logo/stale frames and begins presenting
  only when the replacement first-logo framebuffer is ready;
- the later handover renders the selected MVII-style image only, with the
  trademark and stock colored PS primitive removed;
- BIOS-shell SPU key-on writes are suppressed when **Mute BIOS boot audio** is
  enabled;
- image conversion uses nearest-neighbor scaling and single, non-interleaved TIMs.

Enable **Skip BIOS intro and boot directly to the game** to emit the established
runtime boot-skip configuration (`fast_boot = true`) while retaining the
recompiled BIOS and generated BIOS sources in the app. Normal BIOS boot remains
the default.

The generated proof ZIP includes `bios_branding_patch.json` with every checked
ROM offset, original/replacement instruction, source-image hash, and output BIOS
hash.
