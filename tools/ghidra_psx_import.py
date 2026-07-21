#!/usr/bin/env python3
"""Create a deterministic Ghidra project for a PS-X EXE or raw PS1 BIOS.

The importer mirrors PSXRecomp Studio's verified settings:
MIPS:LE:32:default, BinaryLoader, explicit base address, seeded entry function,
and ExportGameAnalysis proof output.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any, Dict, List

DEFAULT_GHIDRA_HOME = Path.home() / "Tools" / "ghidra_11.3.2_PUBLIC"
REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_DIR = REPO_ROOT / "tools" / "ghidra"


def read_u32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little")


def psx_exe_metadata(path: Path, raw_dir: Path) -> Dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 0x800 or data[:8] != b"PS-X EXE":
        raise ValueError(f"not a PS-X EXE: {path}")
    entry = read_u32(data, 0x10)
    load = read_u32(data, 0x18)
    size = read_u32(data, 0x1C)
    end = 0x800 + size
    if size == 0 or end > len(data):
        raise ValueError(f"invalid PS-X EXE text size 0x{size:X} for {path}")
    raw_dir.mkdir(parents=True, exist_ok=True)
    raw_path = raw_dir / f"{path.name}_no_header.bin"
    raw_path.write_bytes(data[0x800:end])
    return {
        "kind": "exe",
        "source": str(path),
        "import_path": str(raw_path),
        "program_name": raw_path.name,
        "base": load,
        "entry": entry,
        "end": load + size,
        "text_size": size,
    }


def bios_metadata(path: Path, base: int, entry: int | None) -> Dict[str, Any]:
    size = path.stat().st_size
    if size <= 0:
        raise ValueError(f"empty BIOS image: {path}")
    return {
        "kind": "bios",
        "source": str(path),
        "import_path": str(path),
        "program_name": path.name,
        "base": base,
        "entry": base if entry is None else entry,
        "end": base + size,
        "text_size": size,
    }


def ghidra_url(project_dir: Path, project_name: str, program_name: str, address: int) -> str:
    project = (project_dir / project_name).resolve()
    return f"ghidra:{project}?/{program_name}#{address:08X}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--kind", choices=("auto", "exe", "bios"), default="auto")
    parser.add_argument("--ghidra-home", type=Path, default=DEFAULT_GHIDRA_HOME)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0xBFC00000)
    parser.add_argument("--entry", type=lambda value: int(value, 0))
    parser.add_argument("--seed", action="append", default=[], help="additional function address")
    parser.add_argument("--max-cpu", type=int, default=8)
    parser.add_argument("--reuse", action="store_true", help="reuse an existing .gpr without reanalysis")
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    input_path = args.input.expanduser().resolve()
    workspace = args.workspace.expanduser().resolve()
    project_dir = workspace / "ghidra"
    raw_dir = workspace / "project"
    proof_dir = workspace / "proof"
    project_dir.mkdir(parents=True, exist_ok=True)
    proof_dir.mkdir(parents=True, exist_ok=True)
    gpr = project_dir / f"{args.project_name}.gpr"
    lock = project_dir / f"{args.project_name}.lock"
    if lock.exists():
        print(f"Ghidra project is locked: {lock}", file=sys.stderr)
        return 2

    kind = args.kind
    if kind == "auto":
        prefix = input_path.read_bytes()[:8]
        kind = "exe" if prefix == b"PS-X EXE" else "bios"
    metadata = (
        psx_exe_metadata(input_path, raw_dir)
        if kind == "exe"
        else bios_metadata(input_path, args.base, args.entry)
    )
    if args.entry is not None:
        metadata["entry"] = args.entry

    analysis_path = proof_dir / "ghidra_analysis.json"
    url = ghidra_url(project_dir, args.project_name, metadata["program_name"], metadata["entry"])
    if gpr.exists() and args.reuse:
        result = {
            "schema": 1,
            **metadata,
            "project_name": args.project_name,
            "gpr": str(gpr),
            "analysis": str(analysis_path),
            "ghidra_url": url,
            "reused": True,
        }
    else:
        if gpr.exists():
            print(f"project already exists; pass --reuse or choose a fresh workspace: {gpr}", file=sys.stderr)
            return 2
        analyze = args.ghidra_home.expanduser().resolve() / "support" / "analyzeHeadless"
        if not analyze.is_file():
            print(f"analyzeHeadless not found: {analyze}", file=sys.stderr)
            return 2
        command: List[str] = [
            str(analyze),
            str(project_dir),
            args.project_name,
            "-import",
            metadata["import_path"],
            "-overwrite",
            "-processor",
            "MIPS:LE:32:default",
            "-loader",
            "BinaryLoader",
            "-loader-baseAddr",
            f"0x{metadata['base']:08X}",
            "-loader-blockName",
            ".text" if kind == "exe" else "ROM",
            "-scriptPath",
            str(SCRIPT_DIR),
            "-prescript",
            "SeedPsxEntry.java",
            f"0x{metadata['entry']:08X}",
        ]
        seeds = [int(value, 0) for value in args.seed]
        if seeds:
            command.extend(["-prescript", "SeedAdditionalPsxFunctions.java"])
            command.extend(f"0x{value:08X}" for value in seeds)
        command.extend(
            [
                "-postscript",
                "ExportGameAnalysis.java",
                str(analysis_path),
                f"0x{metadata['entry']:08X}",
                f"0x{metadata['base']:08X}",
                f"0x{metadata['end']:08X}",
                "-analysisTimeoutPerFile",
                "1800",
                "-max-cpu",
                str(max(1, args.max_cpu)),
            ]
        )
        completed = subprocess.run(command, check=False)
        if completed.returncode != 0 or not analysis_path.is_file():
            print("Ghidra headless analysis failed or produced no proof manifest", file=sys.stderr)
            return completed.returncode or 1
        result = {
            "schema": 1,
            **metadata,
            "project_name": args.project_name,
            "gpr": str(gpr),
            "analysis": str(analysis_path),
            "ghidra_url": url,
            "reused": False,
        }

    text = json.dumps(result, indent=2)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
