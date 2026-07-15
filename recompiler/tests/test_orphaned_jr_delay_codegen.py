#!/usr/bin/env python3
"""Regression test: an orphaned JR delay slot must execute before return.

A seeded function boundary can place a JR's delay-slot instruction in the next
DiscoveredFunction. The full BIOS emitter must inline that instruction before
publishing the JR target; otherwise stack-restoring delay slots are skipped and
the caller restores registers from the wrong frame.

This test synthesizes a 512 KiB flat BIOS with exactly that boundary:

  BFC00000  addiu sp,sp,-40
  BFC00004  sw    ra,36(sp)
  BFC00008  jr    ra
  BFC0000C  addiu sp,sp,40    # separately seeded orphaned delay slot
  BFC00010  jr    ra
  BFC00014  nop

Usage: python3 test_orphaned_jr_delay_codegen.py [--recompiler PATH]
"""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BIOS_BASE = 0xBFC00000
BIOS_SIZE = 512 * 1024


def default_recompiler() -> str:
    here = Path(__file__).resolve().parent
    base = here.parent / "build" / "psxrecomp-bios"
    if base.is_file():
        return str(base)
    exe = Path(str(base) + ".exe")
    return str(exe)


def build_fixture(tmp: Path) -> tuple[Path, Path, Path]:
    rom = bytearray(BIOS_SIZE)
    words = [
        0x27BDFFD8,  # addiu sp,sp,-40
        0xAFBF0024,  # sw ra,36(sp)
        0x03E00008,  # jr ra
        0x27BD0028,  # addiu sp,sp,40 (orphaned delay slot)
        0x03E00008,  # next function: jr ra
        0x00000000,  # nop
    ]
    for i, word in enumerate(words):
        struct.pack_into("<I", rom, i * 4, word)

    bios = tmp / "bios.bin"
    seeds = tmp / "seeds.json"
    out_dir = tmp / "out"
    bios.write_bytes(rom)
    out_dir.mkdir()

    seed_rows = [
        {"address": "0xBFC00000", "label": "caller", "rationale": "synthetic test"},
        {"address": "0xBFC0000C", "label": "split_delay", "rationale": "synthetic boundary"},
        {"address": "0xBFC00010", "label": "next", "rationale": "synthetic test"},
    ]
    seeds.write_text(json.dumps({
        "schema": "psxrecomp orphaned JR delay regression",
        "source": "synthetic",
        "seed_count": len(seed_rows),
        "seeds": seed_rows,
    }, indent=2))
    return bios, seeds, out_dir


def function_body(source: str, name: str) -> str:
    marker = f"void {name}(CPUState* cpu) {{"
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing generated function {name}")
    end = source.find("\nvoid ", start + len(marker))
    return source[start:] if end < 0 else source[start:end]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", default=default_recompiler())
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler} (build psxrecomp-bios first)")

    with tempfile.TemporaryDirectory(prefix="psx-orphan-jr-") as td:
        tmp = Path(td)
        bios, seeds, out_dir = build_fixture(tmp)
        proc = subprocess.run(
            [args.recompiler, str(bios), str(out_dir), "--emit-full", str(seeds)],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            raise SystemExit(f"recompiler failed:\n{proc.stdout}\n{proc.stderr}")
        generated = list(out_dir.glob("*_full.c"))
        if len(generated) != 1:
            raise SystemExit(f"expected one generated *_full.c, found {generated}")
        body = function_body(generated[0].read_text(), "func_1FC00000")

    delay_marker = "DELAY (orphaned) 0xBFC0000C: 27BD0028"
    stack_restore = "cpu->gpr[29] = (uint32_t)((int32_t)cpu->gpr[29] + (40));"
    publish = "cpu->pc = cpu->gpr[31]; return;"
    failures = []
    if delay_marker not in body:
        failures.append("generated caller does not inline the orphaned JR delay slot")
    if stack_restore not in body:
        failures.append("generated caller does not execute the stack-restoring delay instruction")
    if publish not in body:
        failures.append("generated caller does not publish the JR target")
    if stack_restore in body and publish in body and body.index(stack_restore) > body.index(publish):
        failures.append("JR target is published before the delay-slot stack restore")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("PASS: orphaned JR delay slot executes before the return target is published.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
