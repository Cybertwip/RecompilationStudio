#!/usr/bin/env python3
"""Reliable arbitrary-command client for the PSXRecomp TCP debug protocol.

Examples:
  python3 tools/runtime_batch.py --port 4370 call gpu_state
  python3 tools/runtime_batch.py --port 4470 call cdrom_command_history count=64
  python3 tools/runtime_batch.py --port 4470 batch --file requests.json --output evidence.json

The batch file is either a JSON list of request objects or
{"requests": [ ... ]}. Each request may contain any server command/fields.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List

try:
    from psx_debug_protocol import parse_fields, query, wait_for_port
except ImportError:
    from tools.psx_debug_protocol import parse_fields, query, wait_for_port


def load_requests(path: Path) -> List[Dict[str, Any]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(document, dict):
        document = document.get("requests")
    if not isinstance(document, list) or not all(isinstance(item, dict) for item in document):
        raise ValueError("batch file must be a request list or an object containing a request list")
    return [dict(item) for item in document]


def emit(document: Any, output: Path | None, pretty: bool) -> None:
    text = json.dumps(document, indent=2 if pretty else None, sort_keys=pretty)
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text + "\n", encoding="utf-8")
    print(text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4370)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--wait", type=float, default=0.0, help="wait this many seconds for the port")
    parser.add_argument("--compact", action="store_true")
    sub = parser.add_subparsers(dest="mode", required=True)

    call = sub.add_parser("call", help="send one arbitrary command")
    call.add_argument("command")
    call.add_argument("fields", nargs="*", help="arbitrary key=value fields")
    call.add_argument("--output", type=Path)

    batch = sub.add_parser("batch", help="send requests from a JSON file")
    batch.add_argument("--file", type=Path, required=True)
    batch.add_argument("--output", type=Path)

    args = parser.parse_args()
    if args.wait and not wait_for_port(args.host, args.port, args.wait):
        print(f"runtime debug port {args.host}:{args.port} did not become reachable", file=sys.stderr)
        return 2

    if args.mode == "call":
        request: Dict[str, Any] = {"cmd": args.command}
        request.update(parse_fields(args.fields))
        response = query(args.host, args.port, request, args.timeout)
        emit(response, args.output, not args.compact)
        return 0 if response.get("ok") else 1

    requests = load_requests(args.file)
    responses = []
    failed = False
    for index, request in enumerate(requests, 1):
        request.setdefault("id", index)
        response = query(args.host, args.port, request, args.timeout)
        responses.append({"request": request, "response": response})
        failed = failed or not bool(response.get("ok"))
    document = {
        "schema": 1,
        "host": args.host,
        "port": args.port,
        "results": responses,
    }
    emit(document, args.output, not args.compact)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
