# PSXRecomp Studio third-party components

The complete license texts are vendored with each component.

| Component | Version | License | Location |
|---|---:|---|---|
| Oclero Qlementine | 1.3.0 | MIT | `3rdparty/oclero/qlementine/` |
| Oclero Qlementine Icons | 1.10.0 | MIT | `3rdparty/oclero/qlementine-icons/` |
| Oclero QtAppInstanceManager | 1.3.0 | MIT | `3rdparty/oclero/qtappinstancemanager/` |
| QuaZip | 1.5 | LGPL-2.1 with static-linking exception | `3rdparty/quazip/` |
| MiniZip, within QuaZip | bundled with QuaZip | zlib-style license | `3rdparty/quazip/quazip/` |

The vendored QuaZip snapshot has one source-compatibility adjustment in
`quazip/minizip_crypt.h`: when compiled as C with Qt 6.11, the file defines the
`QUAZIP_UNUSED` compiler attribute locally instead of including the C++ QtGlobal
header. ZIP behavior is unchanged.

The dark and light theme JSON files are derived from the MIT-licensed
Qlementine themes used by the NeoGeo Hub reference application.

Generated Linux exports download checksum-pinned SDL2 2.32.10 headers and the
matching `pysdl2-dll` manylinux x86-64 runtime. Studio installs the SDL2 zlib
license and the `pysdl2-dll` MPL-2.0 license into each Linux package.
