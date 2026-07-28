# Virtua integration for PSXRecomp

This directory contains only PSXRecomp-owned Virtua integration code:

- the thin ARM toolchain and PowerEngine path resolver under `CMake/`;
- the PSXRecomp/MVII native-bridge contract and tiny launchers under `include/`
  and `native/`;
- the PSX runtime's SDL compatibility backend under `sdl/`.

PowerEngine is consumed **in place** through `POWERENGINE_ROOT`. PSXRecomp does
not copy or vendor PowerEngine's `External/Virtua/Dash`, Go packager, ABI
headers, MVII POSIX shim, ARM llvm-libc/libc++ sysroot, or compiler-rt archive.
The matching compiler bundle is supplied through `VIRTUA_LLVM_ROOT`; the CMake
resolver locates its PowerEngine ARM runtime artifacts and fails closed if they
are unavailable.

Configure a Virtua ARM build with both roots:

```sh
cmake -S <source> -B <build> -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
  -DPOWERENGINE_ROOT=/absolute/path/to/PowerEngine \
  -DVIRTUA_LLVM_ROOT=/absolute/path/to/PowerEngine/compiler-bundle \
  -DCMAKE_BUILD_TYPE=Release
```
