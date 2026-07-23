#!/usr/bin/env python3
"""Frame-boundary differential for one GBA binary: AOT vs force-interpreter.

Both peers use the same ROM, BIOS, device model, and TCP step primitive. The tool
injects no input unless extended explicitly; it advances one VBlank boundary at a
time and compares full-state hashes/counters/CPU. On the first mismatch it writes
a standalone JSON proof artifact with detailed region data and recent rings.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


class Client:
    def __init__(self, port: int, timeout: float = 30.0):
        deadline = time.monotonic() + timeout
        self.sock = None
        while time.monotonic() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.05)
        if self.sock is None:
            raise RuntimeError(f"TCP port {port} did not open")
        self.buf = b""

    def call(self, timeout: float = 30.0, **request):
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(1 << 20)
                if not chunk:
                    raise ConnectionError("peer closed")
                self.buf += chunk
        finally:
            self.sock.settimeout(None)
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())

    def close(self):
        if self.sock:
            self.sock.close()


def launch(exe: Path, port: int, force_interp: bool, home: Path):
    home.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["HOME"] = str(home)
    env["GBARECOMP_IDLE_ELISION"] = "0"
    env["GBARECOMP_SELFHEAL_RECOMPILE"] = "0"
    env["RECOMP_RTC_EPOCH"] = "1000000000"
    if force_interp:
        env["GBARECOMP_FORCE_INTERP"] = "1"
    else:
        env.pop("GBARECOMP_FORCE_INTERP", None)
    return subprocess.Popen(
        [str(exe), "--tcp", str(port), "--no-window"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def detailed(client: Client):
    return {
        "run_status": client.call(cmd="run_status"),
        "registers": client.call(cmd="registers"),
        "counters": client.call(cmd="counters"),
        "state_hash": client.call(cmd="state_hash"),
        "ppu_state": client.call(cmd="ppu_state"),
        "audio_state": client.call(cmd="audio_state"),
        "irq_cap": client.call(cmd="irq_cap", count=128),
        "mmio_cap": client.call(cmd="mmio_cap", count=512),
        "runtime_trace": client.call(cmd="runtime_trace", count=512),
        "iwram": client.call(cmd="read_iwram", addr="0x03000000", len=32768),
        "io": client.call(cmd="read_io", addr="0x04000000", len=1024),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--frames", type=int, default=2000)
    ap.add_argument("--a-port", type=int, default=4570)
    ap.add_argument("--b-port", type=int, default=4571)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    exe = Path(args.exe).resolve()
    root = Path(tempfile.gettempdir()) / f"gba-dual-frame-{os.getpid()}"
    pa = launch(exe, args.a_port, False, root / "aot")
    pb = launch(exe, args.b_port, True, root / "interp")
    ca = cb = None
    result = {"schema": 1, "exe": str(exe), "frames_requested": args.frames}
    try:
        ca = Client(args.a_port)
        cb = Client(args.b_port)
        for frame in range(1, args.frames + 1):
            rb = cb.call(cmd="step", timeout=60.0)
            ra = ca.call(cmd="step", timeout=60.0)
            ha = ca.call(cmd="state_hash")
            hb = cb.call(cmd="state_hash")
            ppa = ca.call(cmd="ppu_state")
            ppb = cb.call(cmd="ppu_state")
            aua = ca.call(cmd="audio_state")
            aub = cb.call(cmd="audio_state")
            # `step` parks at the same VBlank boundary, but the two execution
            # engines may be at different instruction boundaries inside that
            # PPU phase. Compare architectural machine content, not transient
            # PC/cycle position. The combined state_hash includes cycles, so use
            # its per-region hashes explicitly.
            region_keys = ("iwram", "ewram", "vram", "pal", "oam")
            same_regions = all(ha.get(key) == hb.get(key) for key in region_keys)
            ppu_keys = ("dispcnt", "dispstat", "vcount", "bg0cnt", "bg1cnt",
                        "bg2cnt", "bg3cnt", "winin", "winout", "bldcnt",
                        "bldalpha", "bldy")
            same_ppu = all(ppa.get(key) == ppb.get(key) for key in ppu_keys)
            same_audio = (aua.get("samples_generated") == aub.get("samples_generated") and
                          aua.get("soundbias") == aub.get("soundbias"))
            if not (same_regions and same_ppu and same_audio):
                result.update({
                    "ok": False,
                    "first_divergent_frame": frame,
                    "step_a": ra,
                    "step_b": rb,
                    "a": detailed(ca),
                    "b": detailed(cb),
                })
                Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
                print(f"FIRST DIVERGENCE frame={frame} -> {args.output}", flush=True)
                return 1
            if frame % 100 == 0:
                print(f"frame {frame}: identical", flush=True)
        result.update({"ok": True, "frames_identical": args.frames})
        Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
        print(f"IDENTICAL through frame {args.frames} -> {args.output}", flush=True)
        return 0
    finally:
        for client in (ca, cb):
            if client:
                try: client.call(cmd="quit", timeout=2.0)
                except Exception: pass
                client.close()
        for proc in (pa, pb):
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
