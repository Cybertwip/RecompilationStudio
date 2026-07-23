# PSXRecomp Studio integration

This tree is the isolated C++ GBA platform core used by PSXRecomp Studio. Studio
copies it into each generated GBA source repository, invokes `gba_recompile`,
and links the generated shards against its libraries. The repository-root
`runtime/` is not used or modified by the GBA path.

Studio supplies SDL2 through `GBARECOMP_SDL2_INCLUDE_DIRS` and
`GBARECOMP_SDL2_LIBRARIES`, keeps `GBARECOMP_BUILD_ORACLE=OFF`, and exposes the
final game target as `psx-runtime` solely to preserve the existing CI/package
protocol. The produced executable is still the GBA C++ runtime.

See `studio/GBA_CPP_PORT_PLAN.md` and `studio/GBA_CPP_PORT_CHANGES.md`.
