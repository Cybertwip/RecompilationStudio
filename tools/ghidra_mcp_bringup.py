#!/usr/bin/env python3
"""Deterministic, reuse-first bring-up for GhidraGo + GhidraMCP.

Rules enforced by this tool:
- Probe and reuse an existing Ghidra HTTP endpoint on 8080 first.
- Reuse an existing Python MCP bridge on 8081; a bridge survives Ghidra restarts.
- Never send GhidraGo while 8080 is healthy, because that can open a second
  MCP-owning CodeBrowser and cause an address-in-use failure.
- Launch Ghidra through support/launch.sh in foreground mode, not ghidraRun's
  background wrapper.
- Remove stale locks only with --repair-stale-locks and only when no Ghidra
  process is running.
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, List
from urllib.error import URLError
from urllib.request import urlopen

DEFAULT_GHIDRA_HOME = Path.home() / "Tools" / "ghidra_11.3.2_PUBLIC"
DEFAULT_BRIDGE_DIR = Path.home() / "Tools" / "GhidraMCP-release-1-4"
REPO_ROOT = Path(__file__).resolve().parents[1]


def tcp_open(host: str, port: int, timeout: float = 0.5) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def ghidra_http_probe(host: str, port: int) -> Dict[str, Any]:
    url = f"http://{host}:{port}/methods?offset=0&limit=1"
    try:
        with urlopen(url, timeout=2.0) as response:
            text = response.read().decode("utf-8", "replace").strip()
        lowered = text.lower()
        healthy = (
            bool(text)
            and "request failed:" not in lowered
            and "no program loaded" not in lowered
            and "no current location" not in lowered
            and not lowered.startswith("error")
        )
        return {"healthy": healthy, "url": url, "response": text}
    except (OSError, URLError) as exc:
        return {"healthy": False, "url": url, "error": str(exc)}


def process_lines() -> List[str]:
    completed = subprocess.run(["ps", "aux"], text=True, capture_output=True, check=False)
    return completed.stdout.splitlines() if completed.returncode == 0 else []


def matching_pids(needle: str) -> List[int]:
    pids: List[int] = []
    for line in process_lines():
        if needle not in line or "ghidra_mcp_bringup.py" in line:
            continue
        fields = line.split(None, 10)
        if len(fields) > 1 and fields[1].isdigit():
            pids.append(int(fields[1]))
    return pids


def listener_dir() -> Path:
    return Path(tempfile.gettempdir()) / f"{getpass.getuser()}-ghidra" / "ghidraGo"


def status_document(host: str, http_port: int, mcp_port: int) -> Dict[str, Any]:
    return {
        "schema": 1,
        "ghidra_http": ghidra_http_probe(host, http_port),
        "mcp_bridge_port_open": tcp_open(host, mcp_port),
        "ghidra_pids": matching_pids("ghidra.GhidraRun"),
        "bridge_pids": matching_pids("bridge_mcp_ghidra.py"),
        "listener_dir": str(listener_dir()),
    }


def wait_http(host: str, port: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ghidra_http_probe(host, port).get("healthy"):
            return True
        time.sleep(0.5)
    return False


def wait_path(path: Path, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.25)
    return False


def repair_locks(gpr: Path) -> List[str]:
    if matching_pids("ghidra.GhidraRun"):
        raise RuntimeError("refusing to remove locks while a Ghidra process is running")
    removed: List[str] = []
    project_base = gpr.with_suffix("")
    candidates = [
        project_base.with_suffix(".lock"),
        Path(str(project_base) + ".lock~"),
        listener_dir() / "listenerLock",
        listener_dir() / "listenerReadyLock",
        listener_dir() / "senderLock",
    ]
    for path in candidates:
        if path.exists():
            path.unlink()
            removed.append(str(path))
    return removed


def start_detached(command: List[str], pid_file: Path) -> int:
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    pid_file.write_text(f"{process.pid}\n", encoding="utf-8")
    return process.pid


def project_url(gpr: Path, program: str, address: str) -> str:
    project = gpr.expanduser().resolve().with_suffix("")
    normalized = address.removeprefix("0x").removeprefix("0X")
    return f"ghidra:{project}?/{program}#{normalized}"


def verify_python_mcp(bridge_dir: Path, host: str, port: int) -> Dict[str, Any]:
    python = bridge_dir / ".venv" / "bin" / "python"
    client = REPO_ROOT / "tools" / "ghidra_mcp_client.py"
    completed = subprocess.run(
        [str(python), str(client), "--url", f"http://{host}:{port}/sse", "health"],
        text=True,
        capture_output=True,
        check=False,
        timeout=20,
    )
    return {
        "healthy": completed.returncode == 0,
        "returncode": completed.returncode,
        "stdout": completed.stdout.strip(),
        "stderr": completed.stderr.strip(),
    }


def start(args: argparse.Namespace) -> int:
    gpr = args.gpr.expanduser().resolve()
    ghidra_home = args.ghidra_home.expanduser().resolve()
    bridge_dir = args.bridge_dir.expanduser().resolve()
    if not gpr.is_file():
        print(f"Ghidra project file not found: {gpr}", file=sys.stderr)
        return 2

    initial = status_document(args.host, args.http_port, args.mcp_port)
    actions: Dict[str, Any] = {"initial": initial, "reused_http": False, "reused_bridge": False}

    if initial["ghidra_http"].get("healthy"):
        actions["reused_http"] = True
        actions["note"] = (
            "Existing Ghidra MCP was reused. No GhidraGo request was sent. "
            "Close/restart Ghidra before switching to a different project; never open a second MCP CodeBrowser."
        )
    else:
        if args.repair_stale_locks:
            actions["removed_locks"] = repair_locks(gpr)
        elif not initial["ghidra_pids"]:
            locks = [
                gpr.with_suffix(".lock"),
                Path(str(gpr.with_suffix("")) + ".lock~"),
                listener_dir() / "listenerReadyLock",
            ]
            existing = [str(path) for path in locks if path.exists()]
            if existing:
                print(
                    "stale-looking Ghidra locks exist while no Ghidra process is running; "
                    "rerun with --repair-stale-locks after reviewing them:\n  " + "\n  ".join(existing),
                    file=sys.stderr,
                )
                return 2

        if not matching_pids("ghidra.GhidraRun"):
            launch = ghidra_home / "support" / "launch.sh"
            command = [str(launch), "fg", "jdk", "Ghidra", "", "", "ghidra.GhidraRun", str(gpr)]
            actions["ghidra_pid"] = start_detached(command, Path("/private/tmp/psxrecomp-ghidra.pid"))

        ready = listener_dir() / "listenerReadyLock"
        if not wait_path(ready, args.timeout):
            print(f"GhidraGo listener did not become ready: {ready}", file=sys.stderr)
            return 1
        url = project_url(gpr, args.program, args.address)
        go = ghidra_home / "support" / "GhidraGo" / "ghidraGo"
        completed = subprocess.run([str(go), url], text=True, capture_output=True, check=False, timeout=args.timeout)
        actions["ghidra_url"] = url
        actions["ghidra_go_returncode"] = completed.returncode
        if completed.returncode != 0:
            print(completed.stdout, file=sys.stderr)
            print(completed.stderr, file=sys.stderr)
            return completed.returncode
        if not wait_http(args.host, args.http_port, args.timeout):
            print("Ghidra opened, but the HTTP MCP endpoint did not become healthy", file=sys.stderr)
            return 1

    if tcp_open(args.host, args.mcp_port):
        actions["reused_bridge"] = True
    else:
        bridge_python = bridge_dir / ".venv" / "bin" / "python"
        bridge_script = bridge_dir / "bridge_mcp_ghidra.py"
        command = [
            str(bridge_python),
            str(bridge_script),
            "--ghidra-server",
            f"http://{args.host}:{args.http_port}/",
            "--transport",
            "sse",
            "--mcp-host",
            args.host,
            "--mcp-port",
            str(args.mcp_port),
        ]
        actions["bridge_pid"] = start_detached(command, Path("/private/tmp/psxrecomp-ghidra-bridge.pid"))
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and not tcp_open(args.host, args.mcp_port):
            time.sleep(0.25)
        if not tcp_open(args.host, args.mcp_port):
            print("Python MCP bridge did not become reachable", file=sys.stderr)
            return 1

    actions["python_mcp"] = verify_python_mcp(bridge_dir, args.host, args.mcp_port)
    actions["final"] = status_document(args.host, args.http_port, args.mcp_port)
    print(json.dumps({"schema": 1, "actions": actions}, indent=2))
    return 0 if actions["python_mcp"]["healthy"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--mcp-port", type=int, default=8081)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    bringup = sub.add_parser("start")
    bringup.add_argument("--ghidra-home", type=Path, default=DEFAULT_GHIDRA_HOME)
    bringup.add_argument("--bridge-dir", type=Path, default=DEFAULT_BRIDGE_DIR)
    bringup.add_argument("--gpr", type=Path, required=True)
    bringup.add_argument("--program", required=True)
    bringup.add_argument("--address", default="0x0")
    bringup.add_argument("--timeout", type=float, default=60.0)
    bringup.add_argument("--repair-stale-locks", action="store_true")
    args = parser.parse_args()

    if args.command == "status":
        document = status_document(args.host, args.http_port, args.mcp_port)
        print(json.dumps(document, indent=2))
        return 0 if document["ghidra_http"].get("healthy") and document["mcp_bridge_port_open"] else 1
    return start(args)


if __name__ == "__main__":
    raise SystemExit(main())
