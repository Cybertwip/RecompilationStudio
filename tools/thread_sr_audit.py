#!/usr/bin/env python3
"""Audit PSX TCB restore/RFE status-register transitions from the TCP rings.

This detects a failure class where a TCB contains a live-form SR (IEc/KUc in
bits 0-1) but the restore path applies RFE to it. RFE clears current mode and
pops bits 2-3, which can silently disable interrupts and strand CD/GPU work.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List

try:
    from psx_debug_protocol import query
except ImportError:
    from tools.psx_debug_protocol import query


def u32(value: Any) -> int:
    if isinstance(value, int):
        return value & 0xFFFFFFFF
    return int(str(value), 0) & 0xFFFFFFFF


def rfe_pop(sr: int) -> int:
    return (sr & 0xFFFFFFC0) | ((sr >> 2) & 0x0F)


def capture(host: str, port: int, timeout: float) -> Dict[str, Any]:
    thread_trace = query(host, port, {"cmd": "thread_trace"}, timeout)
    restores = [entry for entry in thread_trace.get("entries", []) if entry.get("name") == "restore"]
    frames = [int(entry.get("frame", 0)) for entry in restores]
    frame_lo = max(0, min(frames) - 8) if frames else 0
    frame_hi = max(frames) + 64 if frames else 0x7FFFFFFF
    requests = {
        "thread_trace": thread_trace,
        "thread_ctx_ring": query(host, port, {"cmd": "thread_ctx_ring", "count": 256}, timeout),
        "irqctx_ring": query(
            host,
            port,
            {"cmd": "irqctx_ring", "count": 4096, "frame_lo": frame_lo, "frame_hi": frame_hi},
            timeout,
        ),
        "gp1_dump": query(host, port, {"cmd": "gp1_dump", "count": 256, "newest": 1}, timeout),
        "irq_state": query(host, port, {"cmd": "irq_state"}, timeout),
        "gpu_state": query(host, port, {"cmd": "gpu_state"}, timeout),
        "cdrom_state": query(host, port, {"cmd": "cdrom_state"}, timeout),
        "frame": query(host, port, {"cmd": "frame"}, timeout),
    }
    return {"schema": 1, "host": host, "port": port, "capture": requests}


def analyze(document: Dict[str, Any]) -> Dict[str, Any]:
    capture_doc = document.get("capture", document)
    thread_entries = capture_doc.get("thread_trace", {}).get("entries", [])
    findings: List[Dict[str, Any]] = []
    for entry in thread_entries:
        if entry.get("name") != "restore" or "saved_sr" not in entry or "sr" not in entry:
            continue
        saved = u32(entry["saved_sr"])
        live = u32(entry["sr"])
        popped = rfe_pop(saved)
        current_mode_present = saved & 0x3
        finding = {
            "seq": entry.get("seq"),
            "frame": entry.get("frame"),
            "tcb": entry.get("target_tcb") or entry.get("current_tcb"),
            "target_pc": entry.get("target_pc"),
            "saved_sr": f"0x{saved:08X}",
            "live_sr": f"0x{live:08X}",
            "rfe_result": f"0x{popped:08X}",
            "live_matches_rfe": live == popped,
            "saved_sr_is_rfe_ready": current_mode_present == 0,
            "saved_current_mode_bits": current_mode_present,
            "interrupt_enable_lost": bool((saved & 1) and not (live & 1)),
        }
        if not finding["saved_sr_is_rfe_ready"] or finding["interrupt_enable_lost"]:
            findings.append(finding)

    irq_entries = capture_doc.get("irqctx_ring", {}).get("entries", [])
    irq_frames = [int(entry.get("frame", 0)) for entry in irq_entries]
    last_irq_frame = max(irq_frames) if irq_frames else None

    display_disables = []
    for entry in capture_doc.get("gp1_dump", {}).get("entries", []):
        value = u32(entry.get("val", 0))
        if (value >> 24) == 0x03 and (value & 1):
            display_disables.append({
                "seq": entry.get("seq"),
                "frame": entry.get("frame"),
                "value": f"0x{value:08X}",
                "pc": entry.get("pc"),
                "ra": entry.get("ra"),
                "sr": entry.get("sr"),
            })

    irq_state = capture_doc.get("irq_state", {})
    sr_now = u32(irq_state.get("cop0_sr", 0)) if irq_state.get("ok") else 0
    istat = u32(irq_state.get("i_stat", 0)) if irq_state.get("ok") else 0
    imask = u32(irq_state.get("i_mask", 0)) if irq_state.get("ok") else 0
    pending_enabled = istat & imask
    current_frame = int(capture_doc.get("frame", {}).get("frame", 0))
    suspicious_frames = [int(item.get("frame", 0)) for item in findings]
    first_suspicious = min(suspicious_frames) if suspicious_frames else None
    last_suspicious = max(suspicious_frames) if suspicious_frames else None
    irq_stopped_after_suspicious = (
        last_suspicious is not None
        and last_irq_frame is not None
        and last_irq_frame <= last_suspicious
        and current_frame > last_suspicious
    )

    status = "FAIL" if findings and irq_stopped_after_suspicious and pending_enabled and not (sr_now & 1) else "PASS"
    return {
        "schema": 1,
        "status": status,
        "current_frame": current_frame,
        "current_sr": f"0x{sr_now:08X}",
        "current_iec": sr_now & 1,
        "i_stat": f"0x{istat:08X}",
        "i_mask": f"0x{imask:08X}",
        "pending_enabled_irq_bits": f"0x{pending_enabled:08X}",
        "last_irq_frame": last_irq_frame,
        "first_suspicious_restore_frame": first_suspicious,
        "last_suspicious_restore_frame": last_suspicious,
        "irq_stopped_after_suspicious_restore": irq_stopped_after_suspicious,
        "display_disable_events": display_disables,
        "suspicious_restores": findings,
    }


def print_summary(result: Dict[str, Any]) -> None:
    print(f"thread SR audit: {result['status']}")
    print(
        f"  frame={result['current_frame']} SR={result['current_sr']} IEc={result['current_iec']} "
        f"pending_enabled={result['pending_enabled_irq_bits']} last_irq_frame={result['last_irq_frame']}"
    )
    for finding in result["suspicious_restores"]:
        print(
            "  restore "
            f"frame={finding['frame']} tcb={finding['tcb']} pc={finding['target_pc']} "
            f"saved={finding['saved_sr']} rfe={finding['rfe_result']} live={finding['live_sr']} "
            f"rfe_ready={finding['saved_sr_is_rfe_ready']} lost_IEc={finding['interrupt_enable_lost']}"
        )
    for event in result["display_disable_events"]:
        print(f"  display disabled at frame={event['frame']} pc={event['pc']} sr={event['sr']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4370)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--input", type=Path, help="analyze an existing capture instead of a live runtime")
    parser.add_argument("--output", type=Path, help="write capture plus analysis JSON")
    args = parser.parse_args()

    document = json.loads(args.input.read_text(encoding="utf-8")) if args.input else capture(args.host, args.port, args.timeout)
    result = analyze(document)
    combined = dict(document)
    combined["analysis"] = result
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(combined, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print_summary(result)
    return 1 if result["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
