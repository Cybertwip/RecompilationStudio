# Virtua support imported for PSXRecomp

This directory contains the minimum Virtua/MVII userspace ABI, packaging tool,
and ARM runtime material needed by Studio exports and the native ARM bridge
launchers. The source files under `Dash/`, `CMake/`, `binary/`, `include/`, and
`posix-shim/` were copied from:

`/Users/cybertwip/Projects/PowerEngineV3/PowerEngine`

The copied baseline is PowerEngine commit
`1e04e46a69` (July 26, 2026). The ARM sysroot and compiler-rt archive were
produced by that same tree's MVII LLVM runtime targets.

`include/native_bridge.h` is the shared PSXRecomp/MVII device contract. Native
launchers never reinterpret a foreign program in userspace: they open the
packaged image, negotiate `/dev/native0`, and submit an explicit typed launch
request. A missing capability is reported as a hard failure.
