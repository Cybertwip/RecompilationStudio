#!/usr/bin/env python3
"""Shared JSON-over-newline transport helpers for PSXRecomp debug tools."""

from __future__ import annotations

import json
import socket
import time
from typing import Any, Dict, Iterable


def connect(host: str, port: int, timeout: float = 10.0) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def receive_json_object(sock: socket.socket) -> Dict[str, Any]:
    """Read one complete JSON object without assuming it fits in one recv()."""
    buf = bytearray()
    pos = 0
    depth = 0
    in_string = False
    escaped = False
    started = False
    while True:
        chunk = sock.recv(1 << 20)
        if not chunk:
            break
        buf.extend(chunk)
        end = len(buf)
        while pos < end:
            byte = buf[pos]
            if in_string:
                if escaped:
                    escaped = False
                elif byte == 0x5C:  # backslash
                    escaped = True
                elif byte == 0x22:  # quote
                    in_string = False
            elif byte == 0x22:
                in_string = True
            elif byte == 0x7B:  # {
                depth += 1
                started = True
            elif byte == 0x7D:  # }
                depth -= 1
                if started and depth == 0:
                    return json.loads(buf[: pos + 1].decode("utf-8"))
            pos += 1
    text = buf.decode("utf-8").strip()
    if not text:
        raise RuntimeError("debug server closed the connection without a response")
    return json.loads(text)


def query(
    host: str,
    port: int,
    payload: Dict[str, Any],
    timeout: float = 10.0,
) -> Dict[str, Any]:
    request = dict(payload)
    request.setdefault("id", 1)
    with connect(host, port, timeout) as sock:
        sock.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode("utf-8"))
        return receive_json_object(sock)


def wait_for_port(host: str, port: int, timeout: float, interval: float = 0.2) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=min(interval, 1.0)):
                return True
        except OSError:
            time.sleep(interval)
    return False


def parse_value(text: str) -> Any:
    lowered = text.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if lowered == "null":
        return None
    # Preserve hexadecimal values as strings: many runtime handlers use
    # json_get_str() for addresses and ranges.
    if lowered.startswith(("0x", "-0x")):
        return text
    if text and (text.isdigit() or (text[0] == "-" and text[1:].isdigit())):
        return int(text, 10)
    if text.startswith(("[", "{", '"')):
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            pass
    return text


def parse_fields(fields: Iterable[str]) -> Dict[str, Any]:
    parsed: Dict[str, Any] = {}
    for field in fields:
        if "=" not in field:
            raise ValueError(f"expected key=value, got: {field}")
        key, value = field.split("=", 1)
        if not key:
            raise ValueError(f"empty field name in: {field}")
        parsed[key] = parse_value(value)
    return parsed
