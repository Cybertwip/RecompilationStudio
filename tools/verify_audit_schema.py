#!/usr/bin/env python3
"""Validate one or more PSXRecomp audit TOML configs.

Unlike the original B0 smoke test, this validator does not depend on a BIOS
config or sibling Tomba checkout at hard-coded paths. Pass ``--config`` (it may
be repeated), positional config paths, or run it from a project containing
``game.toml``/``audit.toml``.
"""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional

from audit_config import AuditConfig, load


def validate(path: Path, expect_remaps: Optional[int] = None) -> AuditConfig:
    cfg = load(path)
    print(f"\n=== {cfg.config_path} ===")
    print(f"  project_root        = {cfg.project_root}")
    print(f"  program.name        = {cfg.name!r}")
    print(f"  program.rom         = {cfg.rom}")
    print(f"  program.load_address = 0x{cfg.load_address:08X}")
    print(f"  program.text_size   = 0x{cfg.text_size:08X}")

    if not cfg.rom.is_file():
        raise FileNotFoundError(f"configured ROM/EXE does not exist: {cfg.rom}")
    rom_size = cfg.rom.stat().st_size

    print(f"  audit.function_starts = {cfg.function_starts}")
    if cfg.function_starts is not None and not cfg.function_starts.is_file():
        raise FileNotFoundError(
            f"configured function-start manifest does not exist: {cfg.function_starts}"
        )

    print(f"  audit.regions      = {len(cfg.regions)} region(s)")
    for region in cfg.regions:
        if region.rom_start < 0 or region.rom_end <= region.rom_start:
            raise ValueError(
                f"{region.name}: invalid ROM range "
                f"0x{region.rom_start:X}..0x{region.rom_end:X}"
            )
        if region.rom_end > rom_size:
            raise ValueError(
                f"{region.name}: ROM range ends at 0x{region.rom_end:X}, "
                f"past file size 0x{rom_size:X}"
            )
        print(
            f"    - {region.name:8s}  rom 0x{region.rom_start:05X}.."
            f"0x{region.rom_end:05X}  vaddr base 0x{region.vaddr_base:08X}  "
            f"({region.rom_end - region.rom_start} bytes)"
        )

    print(f"  audit.normalize.kseg_mask = 0x{cfg.kseg_mask:08X}")
    print(f"  audit.normalize.remap     = {len(cfg.remaps)} remap(s)")
    if expect_remaps is not None and len(cfg.remaps) != expect_remaps:
        raise ValueError(
            f"expected {expect_remaps} remaps, got {len(cfg.remaps)}"
        )
    for remap in cfg.remaps:
        if remap.from_hi <= remap.from_lo:
            raise ValueError(
                f"invalid remap range 0x{remap.from_lo:X}..0x{remap.from_hi:X}"
            )
        print(
            f"    - 0x{remap.from_lo:08X}..0x{remap.from_hi:08X} "
            f"-> +0x{remap.to_lo:X}  ({remap.description})"
        )

    return cfg


def _default_configs() -> list[Path]:
    for name in ("audit.toml", "game.toml"):
        candidate = Path.cwd() / name
        if candidate.is_file():
            return [candidate]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "configs",
        nargs="*",
        type=Path,
        help="audit/game TOML config paths",
    )
    parser.add_argument(
        "--config",
        action="append",
        default=[],
        type=Path,
        dest="config_options",
        help="audit/game TOML config path; may be repeated",
    )
    parser.add_argument(
        "--expect-remaps",
        type=int,
        default=None,
        help="require this remap count (valid only when checking one config)",
    )
    args = parser.parse_args()

    paths = [*args.config_options, *args.configs]
    if not paths:
        paths = _default_configs()
    if not paths:
        parser.error("no config supplied and no audit.toml/game.toml exists in CWD")
    if args.expect_remaps is not None and len(paths) != 1:
        parser.error("--expect-remaps requires exactly one config")

    for path in paths:
        validate(path, args.expect_remaps)
    print(f"\nOK: {len(paths)} config(s) parse and validate.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
