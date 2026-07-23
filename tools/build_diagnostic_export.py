#!/usr/bin/env python3
"""Rebuild a generated macOS PSXRecomp app as a TCP-enabled diagnostic runtime.

Production Release exports intentionally use PSX_DEBUG_TOOLS=OFF. This tool
reuses the packaged EXE/disc/BIOS/seeds without modifying the app, regenerates
verified C, audits it, and builds a plain executable with PSX_DEBUG_TOOLS=ON.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib
import zlib

REPO_ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str], cwd: Path) -> None:
    print("+", " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}")


def ensure_link(link: Path, target: Path) -> None:
    if link.is_symlink() and link.resolve() == target.resolve():
        return
    if link.exists() or link.is_symlink():
        raise RuntimeError(f"workspace entry already exists with a different target: {link}")
    link.symlink_to(target, target_is_directory=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--framework-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--debug-port", type=int, default=4470)
    parser.add_argument("--jobs", type=int, default=max(1, min(8, os.cpu_count() or 1)))
    parser.add_argument("--reuse-generated", action="store_true")
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    app = args.app.expanduser().resolve()
    resources = app / "Contents" / "Resources"
    config_source = resources / "game.toml"
    if not config_source.is_file():
        print(f"game.toml not found in app resources: {config_source}", file=sys.stderr)
        return 2
    config = tomllib.loads(config_source.read_text(encoding="utf-8"))
    game = config["game"]
    exe_stem = Path(game["exe"]).name
    window_title = config.get("runtime", {}).get("window_title", game.get("name", "PSXRecomp diagnostic"))

    workspace = args.workspace.expanduser().resolve()
    project = workspace / "project"
    generated = project / "generated"
    bios_generated = generated / "bios"
    build = workspace / "build"
    project.mkdir(parents=True, exist_ok=True)
    bios_generated.mkdir(parents=True, exist_ok=True)
    for name in ("game", "disc", "bios", "seeds"):
        ensure_link(project / name, resources / name)
    shutil.copy2(config_source, project / "game.toml")

    framework = args.framework_root.expanduser().resolve()
    recompiler_build = framework / "recompiler" / "build"
    run(
        [
            "cmake",
            "--build",
            str(recompiler_build),
            "--target",
            "psxrecomp-game",
            "psxrecomp-bios",
            "--parallel",
            str(args.jobs),
        ],
        framework,
    )

    game_full = generated / f"{exe_stem}_full.c"
    game_dispatch = generated / f"{exe_stem}_dispatch.c"
    bios_full = bios_generated / "SCPH1001_full.c"
    bios_dispatch = bios_generated / "SCPH1001_dispatch.c"
    if not args.reuse_generated or not all(path.is_file() for path in (game_full, game_dispatch, bios_full, bios_dispatch)):
        run([str(recompiler_build / "psxrecomp-game"), "--config", "game.toml"], project)
        run(
            [
                str(recompiler_build / "psxrecomp-bios"),
                "bios/SCPH1001.BIN",
                "generated/bios",
                "--emit-full",
                str(framework / "recompiler" / "seeds" / "phase2_ghidra_seeds.json"),
            ],
            project,
        )

    # codegen_audit locates the project root by walking for CMakeLists.txt,
    # .gitignore, or .git. Install the project marker before auditing; the full
    # diagnostic CMake project replaces this minimal marker below.
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n", encoding="utf-8"
    )

    audit_path = project / "codegen_audit.json"
    run(
        [
            sys.executable,
            str(framework / "tools" / "codegen_audit.py"),
            "--config",
            str(project / "game.toml"),
            "--json-out",
            str(audit_path),
        ],
        framework,
    )
    audit = json.loads(audit_path.read_text(encoding="utf-8"))
    if audit.get("status") != "CLEAN":
        raise RuntimeError("generated-code audit is not CLEAN")

    bios_bytes = (resources / "bios" / "SCPH1001.BIN").read_bytes()
    bios_crc = zlib.crc32(bios_bytes) & 0xFFFFFFFF
    cmake_text = f'''cmake_minimum_required(VERSION 3.20)
project(PSXRecompDiagnostic C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(PSXRECOMP_ROOT "{framework}" CACHE PATH "" FORCE)
set(PSXRECOMP_SKIP_BIOS_STALE_CHECK ON CACHE BOOL "" FORCE)
set(PSX_LAUNCHER OFF CACHE BOOL "" FORCE)
set(PSX_SETTINGS_MENU OFF CACHE BOOL "" FORCE)
set(PSX_DEBUG_TOOLS ON CACHE BOOL "" FORCE)
set(PSX_MACOS_GIP_GAMEPAD OFF CACHE BOOL "" FORCE)
include(${{PSXRECOMP_ROOT}}/runtime/runtime.cmake)
psxrecomp_add_runtime_target(psx-runtime
  BIOS_GENERATED_FULL_C "{bios_full}"
  BIOS_GENERATED_DISPATCH_C "{bios_dispatch}"
  GAME_GENERATED_FULL_C "{game_full}"
  GAME_GENERATED_DISPATCH_C "{game_dispatch}"
  DEBUG_PORT {args.debug_port}
  WINDOW_TITLE "{window_title} — diagnostic"
  EXE_NAME "psx-runtime-debug"
  DEFAULT_BIOS_PATH "{project / 'bios' / 'SCPH1001.BIN'}"
  DEFAULT_GAME_CONFIG_PATH "{project / 'game.toml'}"
)
target_compile_definitions(psx-runtime PRIVATE PSX_EXPECTED_BIOS_CRC32=0x{bios_crc:08X}u)
if(NOT MSVC)
  target_compile_options(psx-runtime PRIVATE $<$<CONFIG:Release>:-O3>)
endif()
'''
    (project / "CMakeLists.txt").write_text(cmake_text, encoding="utf-8")
    run(
        [
            "cmake",
            "-S",
            str(project),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DPSX_DEBUG_TOOLS=ON",
            "-DPSX_LAUNCHER=OFF",
            "-DPSX_SETTINGS_MENU=OFF",
            "-DPSX_MACOS_GIP_GAMEPAD=OFF",
        ],
        framework,
    )
    run(["cmake", "--build", str(build), "--target", "psx-runtime", "--parallel", str(args.jobs)], framework)

    binary = build / "psx-runtime-debug"
    if not binary.is_file():
        candidates = [path for path in build.iterdir() if path.is_file() and os.access(path, os.X_OK)]
        if len(candidates) == 1:
            binary = candidates[0]
        else:
            raise RuntimeError(f"diagnostic executable not found in {build}")
    result = {
        "schema": 1,
        "app": str(app),
        "workspace": str(workspace),
        "binary": str(binary),
        "game_config": str(project / "game.toml"),
        "debug_port": args.debug_port,
        "codegen_audit": str(audit_path),
        "codegen_status": audit.get("status"),
        "run_example": (
            f"PSX_RUNTIME_DATA_DIR={workspace / 'run'} {binary} "
            f"--headless --no-launcher --debug-port {args.debug_port}"
        ),
    }
    text = json.dumps(result, indent=2)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, KeyError, ValueError) as exc:
        print(f"diagnostic build failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
