# Virtua / MVII platform support

PSXRecomp Studio exposes **Virtua ARM** as a concrete target. The target uses
the Virtua v3 executable format and the cooperative-scheduler flag required by
MVII's ARM process manager.

## Execution modes

| System | Virtua ARM behavior |
|---|---|
| PlayStation | The normal static MIPS→C output is linked to the MVII runtime. Video uses the complete software renderer through `/dev/fb0`, audio uses queued PCM through `/dev/dac0`, and keyboard/input events come from `/dev/input0`. |
| Game Boy Advance | **Native must be checked.** Studio emits a small ARM launcher that opens `game.gba` and submits `MVII_NATIVE_GUEST_GBA_ARM7TDMI` to `/dev/native0`. It does not translate or interpret the ROM. |
| PlayStation Vita | `extra/vita2hos` now builds `vita2mvii.virtua`, which submits `MVII_NATIVE_GUEST_VITA_ARMV7`. The old Horizon/libnx sources are retained only as reference and `external/**` is not built or modified. |
| Nintendo Horizon | `extra/horizon2mvii` submits `MVII_NATIVE_GUEST_HORIZON_AARCH64`. Its transport library maps the exact 13-command horizon-linux/mizu `HZN_SCTL_*` contract to `/dev/horizon0`. No Horizon recompilation is performed. |

## Native bridge ABI

The shared contract is `extra/virtua/include/native_bridge.h`.

- Device: `/dev/native0`
- ABI version: `1`
- Negotiation: `MVII_NATIVE_IOCTL_QUERY`
- Launch: `MVII_NATIVE_IOCTL_LAUNCH`
- Guest kinds: GBA ARM7TDMI, Vita ARMv7, Horizon AArch64

Every launcher first negotiates the ABI and checks the requested guest-kind
capability. Missing support is a hard error; there is no emulator fallback and
no synthesized-success path.

The Horizon service-control transport is documented by
`extra/horizon2mvii/include/horizon_servctl_mvii.h`. Its command values are
byte-for-byte aligned with:

- `Reference/horizon-linux/include/uapi/linux/horizon.h`
- `Reference/horizon-linux/kernel/horizon/servctl.c`
- `Reference/mizu/horizon_servctl.h`

## Bundled toolchain material

`extra/virtua/` carries the required PowerEngine Virtua packager, Dash ABI,
POSIX compatibility surface, ARM LLVM runtime sysroot, framebuffer/audio/input
headers, and CMake helpers. Provenance is recorded in
`extra/virtua/PROVENANCE.json`.

## Verification policy

The builds are compile-and-inspect verified. Vita and Horizon applications were
not launched. `runtime/proofs/virtua_platform_smoke.json` records the exact
artifact hashes, Virtua headers, architecture flags, and cooperative flags used
for the verification run.
