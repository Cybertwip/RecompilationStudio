#!/usr/bin/env python3
"""Capture a manual GBA play session without injecting input.

The tool keeps one connection to the native JSON/TCP surface, records monotonic
wall-time + frame/audio/presentation telemetry, saves exact runtime framebuffers,
captures the actual macOS window, and snapshots the post-resampler host PCM ring.
Every sample is an independent JSON proof artifact; session.json is atomically
updated so a crash does not discard earlier evidence.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import time
import wave
import zlib

try:
    import Quartz
except Exception:
    Quartz = None


class JsonClient:
    def __init__(self, host: str, port: int, timeout: float = 15.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(None)
        self.buf = b""

    def call(self, timeout: float = 5.0, **request):
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(1 << 20)
                if not chunk:
                    raise ConnectionError("runtime closed the TCP connection")
                self.buf += chunk
        finally:
            self.sock.settimeout(None)
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def png_chunk(tag: bytes, data: bytes) -> bytes:
    prefix = struct.pack(">I", len(data)) + tag + data
    return prefix + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_rgb_png(path: Path, rgb: bytes, width: int, height: int):
    expected = width * height * 3
    if len(rgb) != expected:
        raise ValueError(f"RGB payload is {len(rgb)} bytes; expected {expected}")
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgb[y * width * 3:(y + 1) * width * 3]
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(png_chunk(b"IHDR", ihdr))
        handle.write(png_chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        handle.write(png_chunk(b"IEND", b""))
    os.replace(tmp, path)


def write_mono_wav(path: Path, pcm_s16le: bytes, rate: int):
    tmp = path.with_suffix(path.suffix + ".tmp")
    with wave.open(str(tmp), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(pcm_s16le)
    os.replace(tmp, path)


def atomic_json(path: Path, document):
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def find_macos_window(title: str):
    if Quartz is None:
        return None
    options = (Quartz.kCGWindowListOptionOnScreenOnly |
               Quartz.kCGWindowListExcludeDesktopElements)
    windows = Quartz.CGWindowListCopyWindowInfo(options, Quartz.kCGNullWindowID) or []
    exact = []
    partial = []
    needle = title.casefold()
    for window in windows:
        name = str(window.get(Quartz.kCGWindowName, "") or "")
        owner = str(window.get(Quartz.kCGWindowOwnerName, "") or "")
        layer = int(window.get(Quartz.kCGWindowLayer, 0) or 0)
        if layer != 0:
            continue
        item = {
            "id": int(window.get(Quartz.kCGWindowNumber, 0) or 0),
            "owner": owner,
            "name": name,
            "bounds": dict(window.get(Quartz.kCGWindowBounds, {}) or {}),
        }
        if name == title or owner == title:
            exact.append(item)
        elif needle in name.casefold() or needle in owner.casefold():
            partial.append(item)
    candidates = exact or partial
    if not candidates:
        return None
    candidates.sort(key=lambda item: (
        item["name"] != title,
        item["owner"] != title,
        -(float(item["bounds"].get("Width", 0)) *
          float(item["bounds"].get("Height", 0))),
    ))
    return candidates[0]


def capture_macos_window(window_id: int, path: Path):
    completed = subprocess.run(
        ["/usr/sbin/screencapture", "-x", "-o", f"-l{window_id}", str(path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip() or
                           f"screencapture exited {completed.returncode}")


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def parse_pc(value) -> int | None:
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        return None


def valid_gba_exec_pc(pc: int | None) -> bool:
    if pc is None:
        return False
    if 0 <= pc < 0x00004000:       # BIOS
        return True
    region = (pc >> 24) & 0xFF
    return region in (0x02, 0x03) or 0x08 <= region <= 0x0D


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--title", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--interval", type=float, default=0.5,
                        help="telemetry sample cadence in seconds")
    parser.add_argument("--window-every", type=int, default=2,
                        help="capture the actual window every N samples")
    parser.add_argument("--tcp-shot-every", type=int, default=4,
                        help="capture the runtime framebuffer every N samples")
    parser.add_argument("--audio-every", type=int, default=4,
                        help="capture post-resampler host PCM every N samples")
    parser.add_argument("--audio-seconds", type=float, default=2.0,
                        help="trailing host PCM seconds saved per audio capture")
    parser.add_argument("--misses-every", type=int, default=20)
    parser.add_argument("--max-seconds", type=float, default=0.0,
                        help="0 means run until interrupted/runtime exit")
    args = parser.parse_args()

    out_dir = Path(args.out_dir).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    started_wall = utc_now()
    started_mono = time.monotonic()
    session = {
        "schema": 1,
        "title": args.title,
        "host": args.host,
        "port": args.port,
        "started_utc": started_wall,
        "interval_seconds": args.interval,
        "window_every_samples": args.window_every,
        "tcp_shot_every_samples": args.tcp_shot_every,
        "audio_every_samples": args.audio_every,
        "audio_seconds": args.audio_seconds,
        "samples": [],
    }
    atomic_json(out_dir / "session.json", session)

    stop = False

    def request_stop(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    client = JsonClient(args.host, args.port)
    ping = client.call(cmd="ping")
    if not ping.get("ok"):
        raise RuntimeError(f"runtime ping failed: {ping}")

    window = find_macos_window(args.title)
    session["initial_window"] = window
    atomic_json(out_dir / "session.json", session)
    sample_index = 0
    deadline = started_mono
    consecutive_errors = 0

    try:
        while not stop:
            now = time.monotonic()
            elapsed = now - started_mono
            if args.max_seconds > 0 and elapsed >= args.max_seconds:
                break

            sample = {
                "index": sample_index,
                "utc": utc_now(),
                "elapsed_seconds": round(elapsed, 6),
            }
            stem = f"s{sample_index:06d}_t{elapsed:010.3f}"
            try:
                sample["run_status"] = client.call(cmd="run_status")
                sample["counters"] = client.call(cmd="counters")
                sample["host_audio_state"] = client.call(cmd="host_audio_state")
                sample["presentation_state"] = client.call(cmd="presentation_state")
                sample["ppu_state"] = client.call(cmd="ppu_state")
                sample["irq_tail"] = client.call(cmd="irq_cap", count=16)

                pc = parse_pc(sample["run_status"].get("pc"))
                fps = float(sample["presentation_state"].get("fps", 0.0) or 0.0)
                if not valid_gba_exec_pc(pc) or (fps > 0.0 and fps < 45.0):
                    sample["fault_probe"] = {
                        "reason": "invalid_pc" if not valid_gba_exec_pc(pc)
                                  else "presentation_below_45_fps",
                        "registers": client.call(cmd="registers"),
                        "runtime_trace": client.call(cmd="runtime_trace", count=512,
                                                     timeout=10.0),
                        "irq_cap": client.call(cmd="irq_cap", count=256,
                                               timeout=10.0),
                        "mmio_cap": client.call(cmd="mmio_cap", count=512,
                                                timeout=10.0),
                        "irq_stack": client.call(cmd="read_iwram",
                                                 addr="0x03007E00", len=512,
                                                 timeout=10.0),
                        "irq_registers": client.call(cmd="read_io",
                                                     addr="0x04000200", len=16,
                                                     timeout=10.0),
                    }

                if args.misses_every > 0 and sample_index % args.misses_every == 0:
                    sample["misses"] = client.call(cmd="misses", timeout=10.0)

                if args.tcp_shot_every > 0 and sample_index % args.tcp_shot_every == 0:
                    shot = client.call(cmd="screenshot", timeout=10.0)
                    if shot.get("ok"):
                        shot_name = stem + "_runtime.png"
                        write_rgb_png(out_dir / shot_name,
                                      bytes.fromhex(shot["data"]),
                                      int(shot["w"]), int(shot["h"]))
                        sample["runtime_screenshot"] = shot_name

                if args.audio_every > 0 and sample_index % args.audio_every == 0:
                    state = sample["host_audio_state"]
                    rate = int(state.get("host_rate", 0) or 0)
                    if rate > 0:
                        wanted = max(1, int(rate * args.audio_seconds))
                        audio = client.call(cmd="host_audio_cap", count=wanted,
                                            timeout=15.0)
                        if audio.get("ok"):
                            audio_name = stem + "_host.wav"
                            write_mono_wav(out_dir / audio_name,
                                           bytes.fromhex(audio["pcm_s16le"]),
                                           int(audio["rate"]))
                            sample["host_audio_capture"] = {
                                "path": audio_name,
                                "rate": audio["rate"],
                                "count": audio["count"],
                                "first": audio["first"],
                                "head": audio["head"],
                            }

                if args.window_every > 0 and sample_index % args.window_every == 0:
                    current = find_macos_window(args.title) or window
                    if current:
                        window = current
                        window_name = stem + "_window.png"
                        capture_macos_window(int(current["id"]),
                                             out_dir / window_name)
                        sample["window_screenshot"] = window_name
                        sample["window"] = current
                    else:
                        sample["window_error"] = "window not found"

                consecutive_errors = 0
            except Exception as exc:
                consecutive_errors += 1
                sample["error"] = f"{type(exc).__name__}: {exc}"

            frame = sample.get("run_status", {}).get("frame", "unknown")
            sample_path = out_dir / f"sample_{sample_index:06d}.json"
            atomic_json(sample_path, sample)
            session["samples"].append({
                "index": sample_index,
                "elapsed_seconds": sample["elapsed_seconds"],
                "frame": frame,
                "artifact": sample_path.name,
                "error": sample.get("error"),
            })
            session["last_sample_utc"] = sample["utc"]
            session["last_elapsed_seconds"] = sample["elapsed_seconds"]
            atomic_json(out_dir / "session.json", session)
            print(f"capture {sample_index:06d}  t={elapsed:9.3f}s  frame={frame}",
                  flush=True)

            sample_index += 1
            if consecutive_errors >= 3:
                session["ended_reason"] = "three consecutive capture errors"
                break
            deadline += max(args.interval, 0.05)
            delay = deadline - time.monotonic()
            if delay > 0:
                time.sleep(delay)
    finally:
        client.close()
        session["ended_utc"] = utc_now()
        session["ended_elapsed_seconds"] = round(time.monotonic() - started_mono, 6)
        session.setdefault("ended_reason", "interrupted" if stop else "completed")
        atomic_json(out_dir / "session.json", session)

    print(f"capture artifacts: {out_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
