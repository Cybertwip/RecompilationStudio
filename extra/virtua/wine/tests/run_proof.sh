#!/bin/sh
# Build a real ARMv7 PIE with the PowerEngine toolchain and run the vwine loader
# over it on this host.
#
# The guest is ARM code and the host usually is not, so nothing here EXECUTES
# the image -- see the note at the top of test_loader.c. What it does check is
# every decision the loader makes before the branch: placement, R_ARM_RELATIVE
# arithmetic against the guest's real link-time symbol values, import binding,
# and the unresolved-import report. That is where load-time bugs live.
#
#   POWERENGINE_ROOT   PowerEngine checkout (default: ~/Projects/PowerEngineV3/PowerEngine)
#
# On a 64-bit host the loader refuses to place a 32-bit guest above 4 GB, which
# is correct and is what the target guarantees for free. macOS reserves the
# whole low 4 GB as __PAGEZERO, so the test binary is linked with a small one to
# make that region available; on ARMv7 none of this applies.

set -e

ROOT="${POWERENGINE_ROOT:-$HOME/Projects/PowerEngineV3/PowerEngine}"
CC="$ROOT/build/Release/package/bin/Release/bundles/compiler/bin/compiler"
LD="$ROOT/build/Release/package/bin/Release/bundles/compiler/bin/ld.lld"
SYSROOT="$ROOT/build/Release/stage/MVII/toolchains/llvm-runtimes/install/arm-eabi"

HERE="$(cd "$(dirname "$0")" && pwd)"
WINE="$HERE/.."
OUT="${TMPDIR:-/tmp}/vwine-proof"
mkdir -p "$OUT"

[ -x "$CC" ] || { echo "no PowerEngine compiler at $CC" >&2; exit 2; }

echo "== building the ARMv7 guest =="
"$CC" --target=armv7a-none-eabi --sysroot="$SYSROOT" -mcpu=cortex-a7 -marm \
      -mfloat-abi=soft -ffreestanding -fPIC -fno-stack-protector \
      -c "$HERE/guest.c" -o "$OUT/guest.o"
"$LD" -shared --build-id=none -z norelro -o "$OUT/guest.so" "$OUT/guest.o"

echo "== building the loader proof for this host =="
PAGEZERO=""
case "$(uname -s)" in
    Darwin) PAGEZERO="-Wl,-pagezero_size,0x1000" ;;
esac
cc -std=c11 -Wall -Wextra -g -O0 -I"$WINE/include" \
   "$HERE/test_loader.c" "$WINE"/source/vwine_*.c $PAGEZERO -o "$OUT/test_loader"

echo "== running =="
"$OUT/test_loader" "$OUT/guest.so"
