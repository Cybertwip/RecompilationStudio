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
| PlayStation Vita | `extra/vita2hos` builds `vita2mvii.virtua`: loads the SELF/VPK, relocates it, binds each import to a host reimplementation of the Vita's kernel API, and branches. |
| Nintendo Horizon | `extra/horizon2mvii` builds `horizon2mvii.virtua`: loads an NRO/NSO, relocates it, and runs it against a reimplementation of Horizon's kernel, SVC layer and HIPC/CMIF IPC. 32-bit only — see below. |

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

## What the front-ends refuse, and why that is the design

- **AArch64 Horizon modules.** Nearly all Switch software is 64-bit and a
  Cortex-A7 cannot execute one AArch64 instruction, so that is a different
  project, not a harder case. `horizon_image_probe_arch` requires two independent
  signals — the entry branch encoding and the shape of `.dynamic` — to agree, and
  refuses when they do not.
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
