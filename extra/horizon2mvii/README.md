# horizon2mvii

This directory contains the Horizon→MVII wiring derived from the two reference
implementations supplied in `Reference/`:

- **horizon-linux**: Horizon binary header and `HZN_SCTL_*` syscall contract.
- **mizu**: native service process, loader, IPC buffer, and service-manager use
  of that contract.

The port has two explicit surfaces:

1. `horizon2mvii.virtua` opens an NRO/NSO/NCA/NSP image and submits it through
   `/dev/native0` as `MVII_NATIVE_GUEST_HORIZON_AARCH64`.
2. `horizon-mvii-transport` replaces Linux's `horizon_servctl(502)` syscall
   with the versioned `/dev/horizon0` ioctl transport while preserving the
   exact command numbering used by mizu and horizon-linux.

Both surfaces fail closed when MVII lacks the required bridge. They do not
pretend a service request succeeded and do not fall back to CPU emulation.

## Build only

```sh
cmake -S extra/horizon2mvii -B build/horizon2mvii -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/horizon2mvii --target horizon2mvii
```

This produces a cooperative ARM control application. The Horizon guest itself
is AArch64 and therefore requires the corresponding MVII native-bridge
capability; this build step does not launch it.

To stage an application without launching it:

```sh
cmake -S extra/horizon2mvii -B build/horizon2mvii -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DHORIZON2MVII_PAYLOAD=/absolute/path/to/application.nsp
cmake --build build/horizon2mvii --target horizon2mvii
```
