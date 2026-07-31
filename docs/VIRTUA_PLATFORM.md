# Virtua / MVII platform support

PSXRecomp Studio exposes **Virtua ARM** as a concrete target. The target uses
the Virtua v3 executable format and the cooperative-scheduler flag required by
MVII's ARM process manager.

## Execution modes

Every guest here reaches the device by one of two routes, and the choice is made
by the guest's instruction set, not by preference:

* **Static recompilation** when the guest CPU is not the host CPU. The PSX is
  MIPS and the GBA is ARM7TDMI (Thumb-heavy ARMv4); neither runs on a Cortex-A7,
  so the guest's code is translated to C ahead of time and compiled for ARMv7.
* **Direct execution — the WINE model** when the guest CPU *is* the host CPU.
  The Vita is ARMv7-A Cortex-A9 and 32-bit Horizon is ARMv7-A, and the J36 is an
  ARMv7-A Cortex-A7. Same instruction set, so the guest's own instructions run on
  the CPU untranslated. What is missing is not a CPU, it is the guest's operating
  system — so that is what gets reimplemented.

| System | Virtua ARM behavior |
|---|---|
| PlayStation | Static MIPS→C output linked to the MVII runtime. Video uses the complete software renderer through `/dev/fb0`, audio queued PCM through `/dev/dac0`, input from `/dev/input0`. |
| Game Boy Advance | Static recompilation via `extra/gbarecomp`; the recompiled cartridge units and BIOS are linked into `extra/gba-to-mvii`, which builds the emulation half of gbarecomp against MVII's devices. Studio drives this automatically — there is no "Native" checkbox. |
| PlayStation Vita | `extra/vita2hos` builds `vita2mvii.virtua`: loads the SELF/VPK, relocates it, binds each import to a host reimplementation of the Vita's kernel API, and branches. Studio accepts a `.pkg` or an unpacked title directory and stages the whole title out of it, decrypting PFS with the title's own licence — see below. |
| Nintendo Horizon | `extra/horizon2mvii` builds `horizon2mvii.virtua`: loads an NRO/NSO, relocates it, and runs it against a reimplementation of Horizon's kernel, SVC layer and HIPC/CMIF IPC. 32-bit only — see below. Studio accepts a `.nsp` directly and stages the whole ExeFS and RomFS out of it — see below. |

Both direct-execution front-ends are **ARMv7-only** and their CMake files refuse
any other `CMAKE_SYSTEM_PROCESSOR`. That guard is load-bearing rather than
defensive: an x86_64 build would compile every line and then have nothing to
branch to, which is a worse failure than not building because it looks like it
works.

## The shared guest-OS layer

`extra/virtua/wine` (`virtua-wine`) is what the Vita and Horizon front-ends have
in common: the ARM ELF32 mapper and relocator, the guest memory mapper
(`vwine_map` / `vwine_make_executable` / `vwine_sync_icache`), and the
missing-import registry.

`vwine_make_executable` is not optional bookkeeping. A Cortex-A7's I-cache is not
coherent with its D-cache, so an image that has just been relocated must have the
written lines cleaned to the point of unification and the corresponding I-cache
lines invalidated before anything branches into it. Skipping it works right up
until a stale line means executing something other than the relocated code.

**An unresolved import is a hard failure, reported by name, and never a stub.**
Binding unknown imports to a function that returns 0 produces a guest that
starts, runs, and misbehaves somewhere far from the missing call. Refusing at
load time costs one line of output and names the exact symbol.

## Studio opens the shipping containers, it does not refuse them by extension

A Vita title ships as a `.pkg` and a Switch title ships as a `.nsp`. Neither is
the guest module — each is a container with the module somewhere inside — so
Studio reads the container itself rather than telling the user to go find a
loose `eboot.bin` or `main`. `studio/src/VitaPkg.{h,cpp}` and
`studio/src/SwitchNsp.{h,cpp}` are the readers; `studio/src/GuestCrypto.{h,cpp}`
is the AES-128 ECB/CTR/XTS they share, and `studio/src/VitaPfs.{h,cpp}` is the
licence reader and PFS decryption front. `stageGuestApp` in
`studio/src/GuestAppSupport.cpp` is what actually unpacks the container, and it
writes three things into `project/package_inputs/`: the whole guest title as
`data/`, the container's own metadata as `container/`, and the module that is
branched to as `executable`. That last one is a small fraction of a title — a
Switch `main` is under 2% of its package — so `data/` is not an extra, it is the
program. The pipeline hashes the container and the extracted image separately
(`container_sha256` and the image's own SHA-256 both land in the identification
proof), records a SHA-256 for every staged data file, and then **re-hashes all of
them in the built package and checks them again in the delivered export**, so a
package that shipped without its data fails the export instead of succeeding
quietly.

**Vita `.pkg`.** The header, the metadata records, and the item table are read
under the format's own published key. The package names its key type at 0xE4;
types 2–4 derive the AES-128-CTR key by ECB-encrypting the package's `riv` with
the corresponding public key, type 1 uses the key directly. The chosen key has
to prove itself: the item table is only accepted when its offsets land inside
the data area and its names are printable ASCII. From there the item list, the
`sce_sys/param.sfo` (title, title id, category, version) and the plaintext
`sce_pfs/pflist` are all readable.

That is only the outer layer. A `pflist` entry with an empty mark is stored
behind PFS, and PFS is keyed by a **klicensee that belongs to one title** — a
licence for the same game in another region does not open it, and the key is
neither in the container nor derivable from it. Studio reads that key from a
NoNpDrm `work.bin`, a `.rif`, or a zRIF string (all three are the same 0x200-byte
`SceNpDrmLicense`: content id at 0x10, key at 0x50), and it looks in
`PSXRECOMP_VITA_LICENSE`, `PSXRECOMP_VITA_ZRIF`, the title's own
`sce_sys/package/work.bin`, the usual `.rif`/`.zrif` names beside the container
and one level up, and `*/<TITLEID>/sce_sys/package/work.bin`.

**A candidate licence is verified before it is used.** The title's own
`sce_pfs/files.db` header is the oracle: the secret is derived with
`scePfsUtilGetSecret` from that header's `files_salt`, `image_spec` and `key_id`,
and `HMAC-SHA1` over the header — with `header_icv` and `rsa_sig0` zeroed —
has to reproduce `header_icv`. A licence for another title derives a different
signature and is rejected by content id. This is why search *order* is only a
matter of speed: a wrong licence cannot silently decrypt a title to garbage.
Decryption itself runs through the vendored psvpfsparser under
`extra/vita2hos/external/Vita3K/external/psvpfstools`.

Studio also takes an **already-unpacked title** — the `eboot.bin` + `sce_sys/`
tree of a dumped title — selected either as the directory or as its `eboot.bin`.
No `sce_pfs` means no licence is wanted, and Studio says so rather than asking
for one.

**Switch `.nsp`.** The outer PFS0 partition, the `.cnmt.xml` content list and the
ticket all parse with no key at all, so Studio can name the title id, the content
meta type, every NCA in the package and the ticket's rights id and title key
before it needs anything. The NCA header is AES-128-XTS under `header_key`,
which Nintendo does not publish and which is not in this repo; Studio looks in
`PSXRECOMP_SWITCH_KEYS`, next to the package, and in `~/.switch/`. With a
`prod.keys` it decrypts the program NCA header and walks the FS headers to
**both** program sections — the ExeFS and the RomFS.

The architecture comes from the ExeFS `main.npdm`, the process manifest the
console's own loader reads; its flags byte states the instruction set outright.
The entry-instruction probe is kept as a second, independent reading and a
disagreement between the two is reported, but it cannot always answer on its
own: an NSO's `.text` is LZ4 and need not begin with a branch.

The point of both readers is that a refusal names the layer it stopped at rather
than the file extension. The samples in `branding/` are the interesting cases:

- `branding/vita/pkg/terraria.pkg` opens completely — 103 items, content id
  `EP4040-PCSB00405_00-TERRARIA00000001`, `Terraria` / `PCSB00405` / `gd` /
  `01.00` from `param.sfo` — and then stops, because `sce_pfs/pflist` marks 78
  files including `eboot.bin` as PFS-encrypted. No licence for `PCSB00405` exists
  on this machine, so the refusal names the title and lists every place it
  looked. Pointed at the `PCSE00317` licence instead — the same game, the US
  region — the refusal changes to a signature mismatch quoting both digests
  (`files.db` signs its header as `a13b754e…`; that licence derives
  `9594fd52…`). That is the per-title check working, not failing.
- `branding/vita/standard/PCSE00317/` is that US title already decrypted. It is
  accepted whichever way it is selected, wants no licence, and stages as 67
  files / 33,564,888 bytes of `data/`.
- `branding/switch/Undertale.nsp` opens completely with the `prod.keys` beside
  it: 7 entries, title id `010080b00ad66000`, patch id `010080b00ad66800`, NCA3
  at master key revision 4, ExeFS `main` (2,708,936) / `main.npdm` / `rtld` /
  `sdk` / `subsdk0` / `subsdk1`, a 157,904,376-byte RomFS of 216 files, and
  `Undertale` — `8-4, Ltd.` from `control.nacp`. It is then **refused for its
  architecture**: its own `main.npdm` declares 64-bit instructions and a 64-bit
  address space, so it is AArch64 and meets the refusal below. Everything above
  is read first, so the refusal is about the instruction set and not about the
  container or the keys.

`branding/` therefore carries no 32-bit Horizon title, and no end-to-end Switch
export can succeed from the samples in this repo. The staging path itself is
exercised against Undertale's real 180 MB payload with the architecture gate
lifted, which is what keeps "the export carries the whole title" a tested claim
rather than an assumed one.

## How the device starts a package, and what that means for argv

MVII launches a Virtua application **by its `.virtua` file, passing that path as
`argv[0]` and nothing else**. `argc` is 1. There is no command line on the
device, and selecting an application on the dashboard is not a special case to
be handled — it is the only way an application is ever started there.

Both front-ends originally read their guest image out of `argv[1]`, so every
dashboard launch printed its usage line and exited. The device console showed
exactly that, and nothing else, for both systems:

```
| vita2mvii: usage: vita2mvii <eboot.bin | .self | .vpk>
| Virtua process ended without an exit code: pid=1 file=…/Terraria.virtua
| horizon2mvii: usage: horizon2mvii <program.nro | program.nso> [heap-size-bytes]
| Virtua process ended without an exit code: pid=2 file=…/Undertale.virtua
```

The guest is not inside the `.virtua`. Studio stages a package directory, and
the `.virtua` is one file in it, beside the guest:

```
Undertale-Virtua-ARM/
  Undertale.virtua      <- argv[0]; the front-end ELF, wrapped
  executable            <- the guest module the front-end loads
  data/                 <- the rest of the title, staged whole
  game.manifest.json, README.md, AppIcon.png, PSXRecomp-Proof.zip
```

So the guest is `dirname(argv[0]) + "/executable"` and the guest's data root is
`dirname(argv[0]) + "/data"`. Both front-ends need the same two answers, so the
resolver is one function in the shared layer —
`vwine_package_resolve_image` in `extra/virtua/wine/source/vwine_package.c` —
rather than a copy in each. An explicit path on the command line still wins, so
a module can be pointed at directly during bring-up; the package is only
consulted when no path was given, which is the case the device always produces.

A package that lost its `executable` is refused by name, quoting the directory
it looked in — not with a usage line, which describes a command line that does
not exist there. The exporter checks the same file: it is re-hashed in the built
package and again in the delivered export, because it is the one file the
front-end opens with nothing to fall back to.

## What the front-ends refuse, and why that is the design

- **AArch64 Horizon modules.** Nearly all Switch software is 64-bit and a
  Cortex-A7 cannot execute one AArch64 instruction, so that is a different
  project, not a harder case. `horizon_image_probe_arch` requires two independent
  signals — the entry branch encoding and the shape of `.dynamic` — to agree, and
  refuses when they do not. Studio refuses earlier and on better evidence: for an
  NSP it reads `main.npdm`, the manifest the console's own loader acts on, so a
  64-bit title is named as such at identification rather than after a
  180-megabyte export. Undertale is exactly this case.
- **Address aliasing** (`svcMapMemory`, `svcMapSharedMemory` and friends). MVII
  runs a flat 1:1 identity map with no per-process page tables
  (`mt6592_mmu.c`), so there is no second mapping to make. Copying instead would
  break exactly the aliasing the guest relies on.
- **Unbacked services.** Horizon's IPC transport is complete and `sm:` is
  implemented; `hid`, `vi`, `nvdrv` and `fsp-srv` are not. Each would have to be
  built on MVII's own devices, and `horizon_ipc_register_service` is the seam.
  horizon-linux has a path that stubs a response for a session with no handler;
  it is deliberately not copied.
- **Unimplemented SVCs**, which print the full register frame and stop rather
  than return a Result for a service that did not run.

## The 32-bit Horizon SVC ABI is derived, and says so

`Reference/` contains no AArch32 Horizon SVC ABI — horizon-linux's compat path is
an explicit `// horizon TODO 32-bit syscalls`. The AArch64 ABI *is* transcribable
from `Reference/horizon-linux/kernel/horizon/sys.c`, and the 32-bit form is
derived from it. Each derivation (D1 register order, D2 64-bit argument
alignment, D3 `MemoryInfo` layout) is written down with its reasoning and its
falsifier at the top of `extra/horizon2mvii/include/horizon_svc.h`, and the three
call sites where the choice is observable log their raw frame on first call.

## External PowerEngine bindings

Virtua ARM builds require two explicit roots:

- `POWERENGINE_ROOT`: the canonical PowerEngine source checkout. Dash, the MVII
  POSIX headers, Virtua ABI headers, the Go packager, and the ARM linker script
  are consumed directly from this tree.
- `VIRTUA_LLVM_ROOT`: the PowerEngine LLVM/compiler bundle selected in Studio.
  The toolchain resolves the matching ARM llvm-libc/libc++ sysroot and
  compiler-rt output from that PowerEngine build.

`extra/virtua/` contains only PSXRecomp's thin CMake integration, SDL backend and
the shared guest-OS layer. No PowerEngine SDK, Dash tree or shim tree is
duplicated in this repository. The external binding manifest is
`extra/virtua/PROVENANCE.json`.

## Removed: the native-bridge mock

Earlier revisions of this document described a `/dev/native0` launch ioctl
(`MVII_NATIVE_IOCTL_QUERY` / `_LAUNCH`, guest kinds GBA/Vita/Horizon) and a
`/dev/horizon0` service-control transport carrying 13 `HZN_SCTL_*` commands
byte-aligned with horizon-linux and mizu.

**None of it existed.** There is no `/dev/native0` and no `/dev/horizon0`
anywhere in PowerEngine, and no `MVII_NATIVE_*` or `MVII_HORIZON_*` in its
kernel. The command numbering matched its references exactly and still addressed
nothing: these were transports to a kernel-side implementation nobody wrote, so
every launcher would have failed at its first `open()`. They were deleted rather
than ported, and the three front-ends above are what replaced them.

Two files from that design are still in the tree and are no longer referenced by
any build: `extra/virtua/native/` (a standalone CMake project no parent adds) and
`extra/virtua/include/native_bridge.h`. They are recorded under
`orphaned_pending_user_decision` in the smoke proof.

## Verification policy

Builds are compile-and-inspect verified; no guest has been launched on the
device. `runtime/proofs/virtua_platform_smoke.json` records artifact hashes,
Virtua header fields, architecture and cooperative flags, and — for
horizon2mvii — the specific disassembly checks that the naked context-switch
asm, the TPIDRURO accessors, the System-mode guest-entry bracket and the
cache-maintenance sequence each came out as written.
