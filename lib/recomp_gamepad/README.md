# recomp_gamepad

Reusable host-controller support for recompilation runtimes.

- `recomp_gamepad::identity` provides persistent SDL controller identities
  (serial, platform path, then GUID fallback).
- `recomp_gamepad::gip` provides the reconnecting libusb Xbox Game Input
  Protocol backend used for wired Microsoft, PDP, and PowerA controllers that
  SDL cannot expose on macOS.

The GIP target intentionally retains the established `psx_gip_*` C ABI so
existing PSX exports remain compatible while GBA and future runtimes link the
same implementation. Parent projects supply pinned SDL2 and libusb targets.
