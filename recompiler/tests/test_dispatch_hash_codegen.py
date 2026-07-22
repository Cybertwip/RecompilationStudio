#!/usr/bin/env python3
"""Regression and proof replay for the generated CPS dispatch hash."""
from __future__ import annotations

import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def lookup(addr: int, records: list[int], slots: list[int]) -> int | None:
    mask = len(slots) - 1
    slot = (((addr >> 2) * 2654435761) & 0xFFFFFFFF) & mask
    for _ in slots:
        encoded = slots[slot]
        if encoded == 0:
            return None
        index = encoded - 1
        if records[index] == addr:
            return index
        slot = (slot + 1) & mask
    raise AssertionError("dispatch hash has no empty sentinel")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_dispatch_hash_codegen.py <psxrecomp-game>")
    tool = Path(sys.argv[1]).resolve()
    load = 0x80010000
    function_count = 40
    function_stride = 0x200
    code = bytearray(function_count * function_stride)
    seeds: list[int] = []
    for i in range(function_count):
        offset = i * function_stride
        address = load + offset
        seeds.append(address)
        struct.pack_into("<II", code, offset, 0x03E00008, 0x00000000)  # jr ra; nop

    header = bytearray(0x800)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, load)
    struct.pack_into("<I", header, 0x18, load)
    struct.pack_into("<I", header, 0x1C, len(code))
    struct.pack_into("<I", header, 0x30, 0x801FFFF0)

    with tempfile.TemporaryDirectory(prefix="psx-dispatch-hash-") as td:
        root = Path(td)
        (root / ".gitignore").write_text("*\n")
        (root / "game.exe").write_bytes(header + code)
        (root / "seeds.txt").write_text(
            "".join(f"0x{address:08X}\n" for address in seeds)
        )
        (root / "game.toml").write_text(
            f'''[game]
name = "dispatch-hash"
id = "TEST-00003"
exe = "game.exe"
load_address = "0x{load:08X}"
entry_pc = "0x{load:08X}"
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
        )
        result = subprocess.run(
            [str(tool), "--config", str(root / "game.toml")],
            cwd=root,
            text=True,
            capture_output=True,
        )
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            return 1

        generated = root / "generated"
        source = (generated / "game.exe_dispatch.c").read_text()
        manifest = json.loads(
            (generated / "game.exe_dispatch_manifest.json").read_text()
        )

        if "uint32_t lo = 0, hi" in source:
            raise AssertionError("generated dispatcher still contains binary search")
        if "psx_game_canonical_address" not in source:
            raise AssertionError("generated game dispatcher does not canonicalize RAM mirrors")
        if "psx_main_ram_mirror_phys(addr)" not in source:
            raise AssertionError("generated game text-range check ignores RAM mirrors")
        if manifest["lookup"] != "open_addressed_linear_probe":
            raise AssertionError("dispatch manifest names the wrong lookup algorithm")
        count = manifest["record_count"]
        table_size = manifest["table_size"]
        if count < function_count:
            raise AssertionError("seeded functions are missing from dispatch records")
        if table_size < count * 2 or table_size & (table_size - 1):
            raise AssertionError("dispatch hash is not a power-of-two table at <= 1/2 load")
        if manifest["verified_record_lookups"] != count:
            raise AssertionError("generator did not self-verify every dispatch record")

        record_block = re.search(
            r"k_psx_game_dispatch\[\] = \{(.*?)\n\};", source, re.DOTALL
        )
        hash_block = re.search(
            r"k_psx_game_dispatch_hash\[PSX_GAME_DISPATCH_HASH_SIZE\] = \{"
            r"(.*?)\n\};",
            source,
            re.DOTALL,
        )
        if not record_block or not hash_block:
            raise AssertionError("generated dispatch arrays are missing")
        records = [
            int(value, 16)
            for value in re.findall(r"\{0x([0-9A-F]{8})u,", record_block.group(1))
        ]
        slots = [int(value) for value in re.findall(r"(\d+)u", hash_block.group(1))]
        if len(records) != count or len(slots) != table_size:
            raise AssertionError("manifest sizes do not match generated arrays")

        for expected, address in enumerate(records):
            if lookup(address, records, slots) != expected:
                raise AssertionError(f"lookup replay failed for 0x{address:08X}")
        record_set = set(records)
        unknowns = [load + i * function_stride + 0x100 for i in range(function_count)]
        for address in unknowns:
            if address not in record_set and lookup(address, records, slots) is not None:
                raise AssertionError(f"unknown address 0x{address:08X} produced a false hit")

        entries = manifest["entries"]
        if len(entries) != count:
            raise AssertionError("dispatch manifest entry count is inconsistent")
        for index, entry in enumerate(entries):
            if int(entry["address"], 16) != records[index]:
                raise AssertionError("dispatch manifest order differs from record order")
            if slots[entry["slot"]] != index + 1:
                raise AssertionError("manifested hash slot does not contain its record")

    print("dispatch hash codegen regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
