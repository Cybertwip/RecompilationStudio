#!/usr/bin/env python3
"""Regression: a three-instruction A0/B0/C0 call thunk must not absorb trailing data."""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def word_jal(pc: int, target: int) -> int:
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_bios_dispatch_thunk_extent.py <psxrecomp-game>")
    tool = Path(sys.argv[1]).resolve()
    load = 0x80010000
    entry = load
    thunk = load + 0x40
    dynamic = load + 0x80
    code = bytearray(0x100)
    words = {
        0x00: word_jal(entry, thunk),
        0x04: 0x00000000,
        0x08: word_jal(entry + 8, dynamic),
        0x0C: 0x00000000,
        0x10: 0x03E00008,
        0x14: 0x00000000,
        0x40: 0x240A00A0,  # addiu t2,zero,0xA0
        0x44: 0x01400008,  # jr t2
        0x48: 0x24090039,  # addiu t1,zero,0x39 (delay slot)
        0x4C: 0xFFFFFFFF,  # trailing data must not enter the thunk body
        0x50: 0x11111111,
        0x54: 0x55555555,
        0x80: 0xFFFFFFFF,
        0x84: 0x11111111,
        0x88: 0x55555555,
    }
    for offset, value in words.items():
        struct.pack_into("<I", code, offset, value)

    header = bytearray(0x800)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, entry)
    struct.pack_into("<I", header, 0x18, load)
    struct.pack_into("<I", header, 0x1C, len(code))
    struct.pack_into("<I", header, 0x30, 0x801FFFF0)

    with tempfile.TemporaryDirectory(prefix="psx-thunk-test-") as td:
        root = Path(td)
        (root / ".gitignore").write_text("*\n")
        (root / "game.exe").write_bytes(header + code)
        (root / "seeds.txt").write_text(f"0x{entry:08X}\n")
        config = f'''[game]
name = "BIOS thunk extent test"
id = "TEST-00001"
exe = "game.exe"
load_address = "0x{load:08X}"
entry_pc = "0x{entry:08X}"
text_size = "0x{len(code):X}"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
strict = true

[audit]
[[audit.regions]]
name = "Text"
rom_start = "0x800"
rom_end = "0x{0x800 + len(code):X}"
vaddr_base = "0x{load:08X}"
[audit.normalize]
kseg_mask = "0x1FFFFFFF"
'''
        (root / "game.toml").write_text(config)
        result = subprocess.run([str(tool), "--config", str(root / "game.toml")],
                                cwd=root, text=True, capture_output=True)
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            return 1
        ranges = (root / "generated/game.exe_full.ranges").read_text()
        if f"F {thunk:08X}" not in ranges or f"R {thunk:08X} C" not in ranges:
            print(ranges)
            raise SystemExit("BIOS thunk was not emitted as an exact 12-byte function")
        dynamic_manifest = json.loads((root / "generated/game.exe_dynamic_targets.json").read_text())
        targets = {int(item["target"], 16) for item in dynamic_manifest["targets"]}
        if thunk in targets:
            raise SystemExit("BIOS thunk was incorrectly manifested as runtime-installed data")
        if dynamic not in targets:
            raise SystemExit("JAL into classified data was not manifested as runtime-installed code")
    print("BIOS dispatch thunk extent regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
