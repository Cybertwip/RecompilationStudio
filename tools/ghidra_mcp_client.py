#!/usr/bin/env python3
"""Python MCP client for the local GhidraMCP bridge.

The script automatically re-executes with the bridge virtualenv when the
system Python does not have the `mcp` package.

Examples:
  python3 tools/ghidra_mcp_client.py health
  python3 tools/ghidra_mcp_client.py current
  python3 tools/ghidra_mcp_client.py function 0x80101008
  python3 tools/ghidra_mcp_client.py decompile 0x80101008 --out /tmp/entry.c
  python3 tools/ghidra_mcp_client.py call get_xrefs_to address=0x80101008 limit=50
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import sys
from typing import Any, Dict, Iterable, List

DEFAULT_BRIDGE_DIR = Path.home() / "Tools" / "GhidraMCP-release-1-4"


def ensure_mcp_runtime(bridge_dir: Path) -> None:
    try:
        import mcp  # noqa: F401
        return
    except ImportError:
        candidate = bridge_dir / ".venv" / "bin" / "python"
        if candidate.is_file() and Path(sys.executable).resolve() != candidate.resolve():
            os.execv(str(candidate), [str(candidate), str(Path(__file__).resolve()), *sys.argv[1:]])
        raise SystemExit(
            f"Python MCP package is unavailable. Expected bridge virtualenv: {candidate}"
        )


def content_to_python(result: Any) -> List[Any]:
    converted: List[Any] = []
    for item in result.content:
        if hasattr(item, "model_dump"):
            converted.append(item.model_dump())
        elif hasattr(item, "text"):
            converted.append({"type": "text", "text": item.text})
        else:
            converted.append(str(item))
    return converted


def content_text(result: Any) -> str:
    parts = []
    for item in result.content:
        parts.append(getattr(item, "text", str(item)))
    return "\n".join(parts)


def failed_text(text: str) -> bool:
    lowered = text.lower()
    return "request failed:" in lowered or lowered.startswith("error ") or "no program" in lowered


async def run_session(args: argparse.Namespace) -> int:
    from mcp import ClientSession
    from mcp.client.sse import sse_client
    from psx_debug_protocol import parse_fields

    async with sse_client(args.url) as streams:
        async with ClientSession(*streams) as session:
            await session.initialize()

            if args.command == "health":
                methods = await session.call_tool("list_methods", {"offset": 0, "limit": 1})
                current = await session.call_tool("get_current_function", {})
                methods_text = content_text(methods)
                current_text = content_text(current)
                healthy = not failed_text(methods_text) and not failed_text(current_text)
                document = {
                    "schema": 1,
                    "healthy": healthy,
                    "url": args.url,
                    "methods_probe": methods_text,
                    "current_function": current_text,
                }
                print(json.dumps(document, indent=2))
                return 0 if healthy else 1

            if args.command == "tools":
                response = await session.list_tools()
                document = [
                    {"name": tool.name, "description": tool.description, "inputSchema": tool.inputSchema}
                    for tool in response.tools
                ]
                print(json.dumps(document, indent=2))
                return 0

            if args.command == "current":
                result = await session.call_tool("get_current_function", {})
                text = content_text(result)
                print(text)
                return 1 if failed_text(text) else 0

            if args.command == "function":
                tool = "get_function_by_address"
                kwargs = {"address": args.address}
            elif args.command == "decompile":
                tool = "decompile_function_by_address"
                kwargs = {"address": args.address}
            elif args.command == "disassemble":
                tool = "disassemble_function"
                kwargs = {"address": args.address}
            elif args.command == "call":
                tool = args.tool
                kwargs = parse_fields(args.fields)
            elif args.command == "batch":
                addresses: List[str] = list(args.address)
                if args.addresses_file:
                    for line in args.addresses_file.read_text(encoding="utf-8").splitlines():
                        line = line.split("#", 1)[0].strip()
                        if line:
                            addresses.append(line)
                operation_map = {
                    "function": "get_function_by_address",
                    "decompile": "decompile_function_by_address",
                    "disassemble": "disassemble_function",
                }
                tool = operation_map[args.operation]
                args.out_dir.mkdir(parents=True, exist_ok=True)
                failed = False
                manifest = []
                for address in addresses:
                    result = await session.call_tool(tool, {"address": address})
                    text = content_text(result)
                    suffix = ".c" if args.operation == "decompile" else ".txt"
                    output = args.out_dir / f"{address.lower().removeprefix('0x')}{suffix}"
                    output.write_text(text + ("" if text.endswith("\n") else "\n"), encoding="utf-8")
                    bad = failed_text(text)
                    failed = failed or bad
                    manifest.append({"address": address, "output": str(output), "ok": not bad})
                print(json.dumps({"schema": 1, "operation": args.operation, "results": manifest}, indent=2))
                return 1 if failed else 0
            else:
                raise AssertionError(args.command)

            result = await session.call_tool(tool, kwargs)
            text = content_text(result)
            if getattr(args, "out", None):
                args.out.parent.mkdir(parents=True, exist_ok=True)
                args.out.write_text(text + ("" if text.endswith("\n") else "\n"), encoding="utf-8")
            if args.json:
                print(json.dumps({"tool": tool, "arguments": kwargs, "content": content_to_python(result)}, indent=2))
            else:
                print(text)
            return 1 if failed_text(text) else 0


def main() -> int:
    early = argparse.ArgumentParser(add_help=False)
    early.add_argument("--bridge-dir", type=Path, default=DEFAULT_BRIDGE_DIR)
    known, _ = early.parse_known_args()
    ensure_mcp_runtime(known.bridge_dir.expanduser())

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bridge-dir", type=Path, default=DEFAULT_BRIDGE_DIR)
    parser.add_argument("--url", default="http://127.0.0.1:8081/sse")
    parser.add_argument("--json", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("health")
    sub.add_parser("tools")
    sub.add_parser("current")

    for name in ("function", "decompile", "disassemble"):
        command = sub.add_parser(name)
        command.add_argument("address")
        command.add_argument("--out", type=Path)

    call = sub.add_parser("call")
    call.add_argument("tool")
    call.add_argument("fields", nargs="*")

    batch = sub.add_parser("batch")
    batch.add_argument("--operation", choices=("function", "decompile", "disassemble"), required=True)
    batch.add_argument("--address", action="append", default=[])
    batch.add_argument("--addresses-file", type=Path)
    batch.add_argument("--out-dir", type=Path, required=True)

    args = parser.parse_args()
    return asyncio.run(run_session(args))


if __name__ == "__main__":
    raise SystemExit(main())
