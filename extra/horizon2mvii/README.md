# horizon2mvii

Runs 32-bit Switch software on MVII by **executing** it, not emulating it.

AArch32 Horizon is ARMv7-A and the J36 is an ARMv7-A Cortex-A7, so the guest's
own instructions are the host's instructions. Nothing here decodes, translates,
interprets or statically recompiles a single one of them. What the device lacks
is not a CPU, it is Horizon — so Horizon is what this directory reimplements, in
the shape WINE reimplements Win32 on Linux.

This is the same design as `extra/vita2hos` (vita2mvii), and the two share
`extra/virtua/wine` — the mapper, the ARM ELF32 relocator and the
missing-import registry are not guest-specific.

## What it is

Four layers, one file each:

| File | Layer |
| --- | --- |
| `source/horizon_image.c` | NRO0/NSO0 → a mapped, relocated ARM32 image (including the LZ4 block decoder NSO segments need) |
| `source/horizon_kernel.c` | the SVCs, on MVII threads, memory and clock |
| `source/horizon_svc.c` | the `svc` trap handler and register marshalling |
| `source/horizon_ipc.c` | HIPC/CMIF and `sm:` |

`source/horizon_main.c` is the front-end: install the SVC handler, bring up IPC,
load, refuse on any unresolved import, make executable, branch.

A Horizon module is not an ELF file, but its MOD0 header points at an ordinary
ELF `.dynamic`, so everything past that point goes through `virtua-wine` rather
than a second relocator.

## What it is not, and how it says so

**It refuses AArch64.** Nearly all Switch software is 64-bit and a Cortex-A7
cannot execute one AArch64 instruction, so that is a different project, not a
harder case. `horizon_image_probe_arch` requires two independent signals — the
entry branch encoding and the shape of `.dynamic` — to agree, and refuses by
name when they do not. Guessing here means mapping 64-bit code and branching
into it, and the crash that follows says nothing about why.

**It refuses aliasing.** `svcMapMemory`, `svcMapSharedMemory` and friends need
the same physical memory visible at two virtual addresses. MVII runs a flat 1:1
identity map with no per-process page tables (`mt6592_mmu.c`), so there is no
second mapping to make, and copying instead would break exactly the aliasing the
guest is relying on. The refusal names the call and explains the constraint.

**It refuses unbacked services.** The IPC transport is complete and `sm:` is
implemented; `hid`, `vi`, `nvdrv` and `fsp-srv` are not. Each is a substantial
subsystem that would have to be built on MVII's own devices (`/dev/fb0`,
`/dev/input0`, `/dev/dac0`), and `horizon_ipc_register_service` is the seam they
plug into. Until then a request for one fails loudly and by name. horizon-linux
has a path that stubs a response for a session with no handler; it is
deliberately not copied.

**It refuses unimplemented SVCs**, printing the full register frame and
stopping, rather than returning a Result for a service that did not run.

**It refuses an unresolved import**, listing every one. A Horizon module
normally imports nothing — its kernel is `svc` instructions and its services are
IPC — so the list is usually empty, and that is the correct state rather than an
unfinished one.

## The 32-bit ABI is derived, and says where

There is no AArch32 Horizon SVC ABI in `Reference/`: horizon-linux's compat path
is a `// horizon TODO 32-bit syscalls`. The AArch64 ABI is transcribable from
`Reference/horizon-linux/kernel/horizon/sys.c` and the 32-bit form is derived
from it. Every derivation is written down as D1/D2/D3 at the top of
`include/horizon_svc.h`, each with its reasoning and its falsifier, and the
three call sites where the choice is observable log their raw frame on first
call.

## Build

ARMv7 only. The guard in `CMakeLists.txt` is load-bearing: an x86_64 build would
compile every line and then have nothing to branch to, which looks like it works.

Nothing is vendored from PowerEngine — Dash, the POSIX shim, the linker script,
the sysroot and the packager are all resolved out of `POWERENGINE_ROOT` by
`extra/virtua/CMake/VirtuaPowerEngine.cmake`.

```sh
cmake -S extra/horizon2mvii -B build/horizon2mvii -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
  -DPOWERENGINE_ROOT=/absolute/path/to/PowerEngine \
  -DVIRTUA_LLVM_ROOT=/absolute/path/to/PowerEngine/build/Release/package/bin/Release/bundles/compiler \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/horizon2mvii --target horizon2mvii
```

Produces `horizon2mvii.virtua`: a cooperative GUI ARM application.

## Run

The image path is `argv[1]`, and an optional heap reservation is `argv[2]`
(default 64 MiB; reserved whole because Horizon guarantees the heap base never
moves and MVII cannot extend a mapping in place).

```
horizon2mvii <program.nro|program.nso> [heap-size-bytes]
```

`-DHORIZON2MVII_PAYLOAD=/path/to/program.nro` stages the guest beside the
runtime as `executable`. It takes an NRO or NSO — not an NSP or NCA, which are
encrypted archives this front-end does not open.
