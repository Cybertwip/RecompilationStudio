#!/usr/bin/env python3
"""gba_tcp.py — interactive TCP driver CLI for the gbarecomp runtime.

The runtime must already be running with `--tcp <port>` (default 19842). This is
a thin, scriptable client over the always-on debug surface (TCP.md): step the
game, inject input, screenshot to a viewable PNG, read regs / symbols / coverage,
load/save states, dump the fp ring. Built for the MC-HP-002 loading-zone freeze
hunt — a real `step` HANG (an infinite loop inside one step_frame) shows up as a
socket timeout; a long-but-finite overshoot still returns.

Usage (one command per invocation, against a live runtime):
  python oracle/gba_tcp.py keys UP            # set joypad (active-low handled)
  python oracle/gba_tcp.py step 30 --keys UP  # set keys, then step 30 frames
  python oracle/gba_tcp.py step 1 --timeout 6 # step, report HANG on timeout
  python oracle/gba_tcp.py shot /tmp/scr.png  # screenshot -> PNG (view with Read)
  python oracle/gba_tcp.py regs               # CPU registers
  python oracle/gba_tcp.py sym 0x08004286     # PC -> function name
  python oracle/gba_tcp.py misses             # self-heal / coverage state
  python oracle/gba_tcp.py loadstate roms/minishcap_usa.state2
  python oracle/gba_tcp.py savestate /tmp/loadzone.state
  python oracle/gba_tcp.py fp /tmp/fp.bin     # dump fingerprint ring (binary)
  python oracle/gba_tcp.py raw '{"cmd":"frame"}'
"""
from __future__ import annotations
import argparse, json, socket, struct, sys, time, zlib

# GBA KEYINPUT is active-low: a 0 bit = pressed. bit0 A,1 B,2 Sel,3 Start,
# 4 Right,5 Left,6 Up,7 Down,8 R,9 L.
KEYBITS = {"A": 0, "B": 1, "SELECT": 2, "START": 3, "RIGHT": 4, "LEFT": 5,
           "UP": 6, "DOWN": 7, "R": 8, "L": 9}
NONE_KEYINPUT = 0x3FF


def keyinput_from(spec: str) -> int:
    """'UP', 'UP+A', 'none', or a hex/dec literal -> 10-bit active-low value."""
    s = spec.strip().upper()
    if s in ("NONE", "", "RELEASE"):
        return NONE_KEYINPUT
    if s.startswith("0X") or s.isdigit():
        return int(s, 0) & 0x3FF
    val = NONE_KEYINPUT
    for name in s.replace(",", "+").split("+"):
        name = name.strip()
        if name not in KEYBITS:
            raise SystemExit(f"unknown key '{name}'; known: {list(KEYBITS)}")
        val &= ~(1 << KEYBITS[name]) & 0x3FF
    return val


class JsonClient:
    def __init__(self, host, port, connect_timeout=10.0):
        deadline = time.time() + connect_timeout
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError:
                time.sleep(0.1)
        if self.sock is None:
            raise SystemExit(f"can't reach {host}:{port} (is the runtime up?)")
        self.buf = b""

    def call(self, timeout=None, **kw):
        self.sock.sendall(json.dumps(kw).encode() + b"\n")
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                ch = self.sock.recv(1 << 20)
                if not ch:
                    raise RuntimeError("peer closed")
                self.buf += ch
        finally:
            self.sock.settimeout(None)
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())


def write_png(path: str, rgb: bytes, w=240, h=160):
    """Minimal RGB888 PNG encoder (stdlib zlib only) so Read can view it."""
    if len(rgb) < w * h * 3:
        raise SystemExit(f"framebuffer too small: {len(rgb)} < {w*h*3}")
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8-bit, color type 2 (RGB)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19842)
    ap.add_argument("--host", default="127.0.0.1")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("keys"); p.add_argument("spec")
    p = sub.add_parser("step")
    p.add_argument("n", type=int, nargs="?", default=1)
    p.add_argument("--keys", default=None)
    p.add_argument("--timeout", type=float, default=None)
    p.add_argument("--shot-every", type=int, default=0)
    p.add_argument("--shot-dir", default="/tmp")
    p = sub.add_parser("shot"); p.add_argument("path")
    sub.add_parser("regs")
    p = sub.add_parser("sym"); p.add_argument("addr")
    sub.add_parser("misses")
    sub.add_parser("frame")
    p = sub.add_parser("loadstate"); p.add_argument("path")
    p = sub.add_parser("savestate"); p.add_argument("path")
    p = sub.add_parser("fp"); p.add_argument("path")
    p = sub.add_parser("raw"); p.add_argument("json")
    p = sub.add_parser("dumpram")   # dump full IWRAM (works live, even frozen)
    p.add_argument("path")
    p.add_argument("--region", choices=["iwram", "ewram"], default="iwram")
    sub.add_parser("cont")        # free-run (game thread); server stays live
    sub.add_parser("pause")
    sub.add_parser("rstatus")     # run-state / parked / pc / vblank_starts
    p = sub.add_parser("mmio")
    p.add_argument("addr")
    p.add_argument("--count", type=int, default=262144)
    p.add_argument("--start", type=int, default=0)
    p = sub.add_parser("tracepc")
    p.add_argument("pc")
    p.add_argument("--count", type=int, default=4096)
    p = sub.add_parser("paced")
    p.add_argument("--frames", type=int, default=600)
    p.add_argument("--hz", type=float, default=59.7275)
    p = sub.add_parser("watchhash")
    p.add_argument("--frames", type=int, default=30)
    p = sub.add_parser("watchmem")
    p.add_argument("addr")
    p.add_argument("--len", type=int, default=16)
    p.add_argument("--frames", type=int, default=30)
    p.add_argument("--region", choices=["iwram", "ewram", "io"], default="iwram")
    # freerun: set keys, continue, then poll run_status while the game free-runs
    # and report the moment guest progress STALLS (vblank_starts stops advancing)
    # — the freeze, observed LIVE on the hung core. Reports frozen pc + regs.
    p = sub.add_parser("freerun")
    p.add_argument("--keys", default=None)
    p.add_argument("--secs", type=float, default=20.0)   # max wall time
    p.add_argument("--stall", type=float, default=3.0)   # vblank-stall → freeze
    args = ap.parse_args()

    c = JsonClient(args.host, args.port)

    if args.cmd == "keys":
        v = keyinput_from(args.spec)
        print(json.dumps(c.call(cmd="set_keyinput", value=v)))
    elif args.cmd == "step":
        if args.keys is not None:
            c.call(cmd="set_keyinput", value=keyinput_from(args.keys))
        for i in range(args.n):
            t0 = time.time()
            try:
                r = c.call(cmd="step", timeout=args.timeout)
            except socket.timeout:
                print(json.dumps({"HANG": True, "at_step": i, "elapsed_s":
                                  round(time.time() - t0, 2),
                                  "msg": "step did not return within timeout — "
                                         "freeze (loop never reaches frame end)"}))
                return
            dt = time.time() - t0
            if dt > 0.5:  # surface slow frames (the freeze onset)
                fr = r.get("frame")
                print(json.dumps({"slow_step": i, "elapsed_s": round(dt, 2),
                                  "frame": fr}))
            if not r.get("ok", True):
                print(json.dumps({"step_failed": i, "resp": r})); return
            if args.shot_every and (i + 1) % args.shot_every == 0:
                s = c.call(cmd="screenshot")
                write_png(f"{args.shot_dir}/step_{i+1:05d}.png",
                          bytes.fromhex(s["data"]))
        print(json.dumps({"ok": True, "stepped": args.n}))
    elif args.cmd == "shot":
        s = c.call(cmd="screenshot")
        write_png(args.path, bytes.fromhex(s["data"]))
        print(json.dumps({"ok": True, "path": args.path, "w": s.get("w"),
                          "h": s.get("h")}))
    elif args.cmd == "regs":
        print(json.dumps(c.call(cmd="registers"), indent=0))
    elif args.cmd == "sym":
        print(json.dumps(c.call(cmd="symbol", addr=args.addr)))
    elif args.cmd == "misses":
        print(json.dumps(c.call(cmd="misses")))
    elif args.cmd == "frame":
        print(json.dumps(c.call(cmd="frame")))
    elif args.cmd == "loadstate":
        print(json.dumps(c.call(cmd="savestate_load", path=args.path)))
    elif args.cmd == "savestate":
        print(json.dumps(c.call(cmd="savestate_save", path=args.path)))
    elif args.cmd == "fp":
        print(json.dumps(c.call(cmd="fp_save", path=args.path)))
    elif args.cmd == "raw":
        print(json.dumps(c.call(**json.loads(args.json))))
    elif args.cmd == "dumpram":
        cmd = "read_" + args.region
        base = 0x03000000 if args.region == "iwram" else 0x02000000
        size = 0x8000 if args.region == "iwram" else 0x40000
        out = bytearray()
        chunk = 0x2000
        for off in range(0, size, chunk):
            r = c.call(cmd=cmd, addr=hex(base + off), len=chunk, timeout=5.0)
            out += bytes.fromhex(r["data"])
        with open(args.path, "wb") as f:
            f.write(out)
        print(json.dumps({"ok": True, "path": args.path, "bytes": len(out),
                          "base": hex(base)}))
    elif args.cmd == "cont":
        print(json.dumps(c.call(cmd="continue")))
    elif args.cmd == "pause":
        print(json.dumps(c.call(cmd="pause")))
    elif args.cmd == "rstatus":
        print(json.dumps(c.call(cmd="run_status")))
    elif args.cmd == "tracepc":
        pc = int(args.pc, 0)
        r = c.call(cmd="runtime_trace", count=args.count, timeout=5.0)
        rows = []
        for row in r.get("entries", []):
            value = row.get("pc", 0)
            if isinstance(value, str): value = int(value, 0)
            if value == pc: rows.append(row)
        print(json.dumps({"ok": True, "pc": hex(pc), "matches": rows,
                          "scanned": len(r.get("entries", []))}))
    elif args.cmd == "paced":
        period = 1.0 / max(args.hz, 1.0)
        t0 = time.monotonic()
        for i in range(args.frames):
            c.call(cmd="step", timeout=5.0)
            deadline = t0 + (i + 1) * period
            delay = deadline - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        print(json.dumps({"ok": True, "frames": args.frames,
                          "elapsed": round(time.monotonic() - t0, 3)}))
    elif args.cmd == "watchhash":
        rows = []
        for frame in range(args.frames + 1):
            h = c.call(cmd="state_hash", timeout=5.0)
            st = c.call(cmd="run_status", timeout=5.0)
            rows.append({"step": frame, "frame": st.get("frame"),
                         "pc": st.get("pc"), "hash": h})
            if frame != args.frames:
                c.call(cmd="step", timeout=10.0)
        print(json.dumps({"ok": True, "rows": rows}))
    elif args.cmd == "watchmem":
        addr = int(args.addr, 0)
        cmd = "read_" + args.region
        rows = []
        for frame in range(args.frames + 1):
            r = c.call(cmd=cmd, addr=hex(addr), len=args.len, timeout=5.0)
            st = c.call(cmd="run_status", timeout=5.0)
            rows.append({"step": frame, "frame": st.get("frame"),
                         "pc": st.get("pc"), "data": r.get("data")})
            if frame != args.frames:
                c.call(cmd="step", timeout=10.0)
        print(json.dumps({"ok": True, "addr": hex(addr), "rows": rows}))
    elif args.cmd == "mmio":
        wanted = int(args.addr, 0)
        cursor = args.start
        remaining = args.count
        matches = []
        total = None
        while remaining > 0:
            take = min(4096, remaining)
            r = c.call(cmd="mmio_cap", start=cursor, count=take, timeout=5.0)
            total = r.get("total", total)
            rows = r.get("entries", [])
            for row in rows:
                addr = row.get("addr", 0)
                if isinstance(addr, str):
                    addr = int(addr, 0)
                if addr == wanted:
                    matches.append(row)
            got = len(rows)
            if got == 0:
                break
            cursor += got
            remaining -= got
            if total is not None and cursor >= total:
                break
        print(json.dumps({"ok": True, "addr": hex(wanted), "matches": matches,
                          "scanned": cursor - args.start, "total": total}))
    elif args.cmd == "freerun":
        if args.keys is not None:
            c.call(cmd="set_keyinput", value=keyinput_from(args.keys))
        c.call(cmd="continue")
        t0 = time.time()
        # Freeze signal: the guest PC stays within a tiny window (a busy-spin
        # loop) for `stall` seconds. NOTE vblank_starts/frame KEEP ADVANCING
        # during the spin (the loop's per-insn runtime_tick still ticks the PPU),
        # so they are NOT a freeze signal — the PC staying put is.
        ref_pc, ref_t = None, time.time()
        while time.time() - t0 < args.secs:
            st = c.call(cmd="run_status", timeout=3.0)
            pc = int(st.get("pc", "0x0"), 16)
            now = time.time()
            if ref_pc is None or abs(pc - ref_pc) > 0x80:
                ref_pc, ref_t = pc, now           # PC moved → not (yet) spinning
            elif now - ref_t >= args.stall:
                regs = c.call(cmd="get_registers", timeout=3.0)
                sym = c.call(cmd="symbol", addr=st.get("pc"), timeout=3.0)
                print(json.dumps({"FREEZE": True,
                                  "spin_window_s": round(now - ref_t, 1),
                                  "status": st, "symbol": sym, "regs": regs}))
                return
            time.sleep(0.25)
        print(json.dumps({"ok": True, "no_freeze_within_s": args.secs,
                          "status": c.call(cmd="run_status")}))


if __name__ == "__main__":
    main()
