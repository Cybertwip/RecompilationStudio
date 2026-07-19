# PSXRecomp Studio

PSXRecomp Studio is the Qt front end for producing self-contained macOS,
Windows, or Linux PlayStation recompilation apps without creating a permanent
per-title repository. A Windows Studio build exports Windows packages and a
Linux Studio build exports Linux packages. macOS can select macOS, Windows,
Linux, or **All**; All queues every available target into one output directory.

It uses Qt 6 Widgets, Oclero Qlementine, Qlementine Icons,
QtAppInstanceManager, and QuaZip. Work happens in a temporary directory; the
selected output directory receives only the final platform package.

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
pick optional icons from the generated list. One selected BIOS and the common
runtime/branding settings apply to the whole queue. A directory containing both
a CUE and its referenced BIN tracks produces one game entry, not one entry per
track; unowned standalone BIN images become their own entries.

On Windows and Linux, Batch exports every listed game for the native host
platform. On macOS, an individual platform does the same for that target, while
**All** exports each listed game for macOS, Windows, and Linux.

## Host tools

Every export requires CMake, Python 3, Ghidra 11.3.2, and OpenJDK 21. Non-Windows
hosts also require Ninja.
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

**Controller pad type** (Hybrid / Analog / D-Pad) is chosen in Studio and baked
into the packaged `game.toml` with `lock_mode = true`, so the in-game settings
menu does not expose pad-type switching for that title.
The resulting package records its actual minimum glibc requirement from the
versioned symbols observed in the delivered executable and bundled SDL2 runtime;
it does not declare or enforce a fixed glibc baseline. The package is also
verified to have no non-system shared-library dependency outside bundled SDL2.

Qt is bundled into the distributed Studio app. Qt is not linked into generated
game apps; those use the PSXRecomp SDL runtime.

## Pipeline

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
12. Copy the verified `.app`, `-Windows`, or `-Linux` package into the selected
    output directory and verify the delivered copy again.

A direct JAL into bytes classified as static data is emitted only as a
runtime-installed target. The bytes remain absent from native C and the runtime
requires dirty-RAM evidence before its interpreter can execute them. Canonical
A0/B0/C0 BIOS call thunks are recognized separately and remain native code.

## Generated app layout

```text
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

Windows exports use:

```text
Game Name-Windows/
  Game Name.exe
  game.toml
  PSXRecomp-Proof.zip
  bios/SCPH1001.BIN
  game/<serial boot EXE>
  disc/<CUE and referenced BIN files, or standalone BIN>
  seeds/ghidra_funcs.txt
```

Linux exports use:

```text
Game Name-Linux/
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
requires the static libusb archive, passes `PSX_MACOS_GIP_GAMEPAD=ON` explicitly
to the generated project, and rejects the export unless the finished executable
contains the native GIP symbols and has no dynamic libusb dependency. Player 1
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
