# PSXRecomp Studio

PSXRecomp Studio is the macOS Qt front end for producing a self-contained,
signed PlayStation recompilation app without creating a permanent per-title
repository.

It uses Qt 6 Widgets, Oclero Qlementine, Qlementine Icons,
QtAppInstanceManager, and QuaZip. Work happens in a temporary directory; the
selected output directory receives only the final `.app` bundle.

## Required inputs

- One CUE sheet and every BIN file it references. Multi-track CUE sheets are
  supported.
- An exact North American `SCPH1001.BIN` image:
  - size: `524288` bytes
  - SHA-256: `71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3`
- A PNG, SVG, or ICNS app icon.
- Optional BIOS branding images: one initial splash and one handoff image.
- The desired runtime window title. This also becomes the macOS app name.
- A password-protected PKCS#12 signing identity (`.pfx` or `.p12`) and its
  password.
- Ghidra 11.3.2 with Java 21.

The certificate password is kept only in memory. Studio validates the source
PFX with OpenSSL 3, re-exports a temporary legacy-compatible PKCS#12 using a
random ASCII password (to avoid macOS Security's PBES2 and Unicode-password
import bugs), imports that identity into a temporary keychain, signs and
verifies the bundle, then deletes all temporary key material and the keychain.

## Host tools

The machine running Studio must provide:

- Xcode command-line tools (`codesign`, `security`, `iconutil`, `otool`)
- CMake and Ninja
- Python 3
- SDL2 development files discoverable through `pkg-config`
- libusb 1.0 development files, including `libusb-1.0.a`, for the default wired
  Xbox/PDP controller export (`brew install libusb`)
- When Homebrew provides SDL2 through `sdl2-compat`, the matching SDL3 runtime
  must be installed; Studio bundles and signs both libraries.
- OpenSSL 3 with `pkcs12 -legacy` support (`brew install openssl@3`)
- Ghidra 11.3.2 and OpenJDK 21

Qt is bundled into the distributed Studio app. Qt is not linked into generated
game apps; those use the PSXRecomp SDL runtime.

## Pipeline

1. Parse the CUE and validate every referenced BIN.
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
10. Compile a Release macOS application bundle, explicitly enable and verify
    the selected wired Xbox/PDP GIP backend, bundle SDL2 and (when needed) SDL3,
    and embed the BIOS, extracted EXE, CUE/BIN files, runtime configuration,
    icon, and a QuaZip proof archive.
11. Import the normalized PFX into an isolated temporary keychain, sign every
    nested Mach-O, and sign the app with Hardened Runtime, a secure timestamp,
    and the narrow `disable-library-validation` entitlement required by
    non-Apple/self-issued code-signing identities. Run strict `codesign`
    verification before and after delivery.
12. Copy the verified `.app` into the selected output directory and verify the
    delivered copy again.

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
      disc/<CUE and referenced BIN files>
```

### macOS wired Xbox/PDP controller export

**Enable wired Xbox/PDP controllers on macOS** is enabled by default. Studio
requires the static libusb archive, passes `PSX_MACOS_GIP_GAMEPAD=ON` explicitly
to the generated project, and rejects the export unless the finished executable
contains the native GIP symbols and has no dynamic libusb dependency. Player 1
uses `p1_device = "auto"`, so SDL controllers remain preferred and the direct USB
backend activates when SDL cannot expose the wired device.

The proof archive contains `macos_gip_controller.json` and records whether the
backend was requested, compiled, statically linked, and symbol-verified.

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
