#!/usr/bin/env python3
"""Audit generated PSXRecomp C, dispatch, range, and data manifests.

The audit is format-aware for both the BIOS two-field dispatch table and the
current game three-field table (address, resume PC, owner function). It fails on
generated fallback bodies, unsupported-instruction markers, unresolved direct
function calls, missing dispatch targets, or code/data manifest overlap.
"""
from __future__ import annotations

import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import audit_config


EXPECTED_TAIL_CALL_MISSES = {
    "SCPH1001 BIOS": {0x00000CF0},  # runtime-installed SIO code (Rule 18)
}


def option_value(argv: list[str], name: str) -> str | None:
    for i, arg in enumerate(argv):
        if arg == name and i + 1 < len(argv):
            return argv[i + 1]
        if arg.startswith(name + "="):
            return arg.split("=", 1)[1]
    return None


def parse_dispatch_entries(text: str) -> set[int]:
    # BIOS: { 0xADDRu, func_X }
    # Game: {0xADDRu, 0xRESUMEu, func_X}
    pattern = re.compile(
        r"\{\s*0x([0-9A-Fa-f]+)u?\s*,"
        r"(?:\s*0x[0-9A-Fa-f]+u?\s*,)?"
        r"\s*func_[0-9A-Fa-f]+(?:_cont_[0-9A-Fa-f]+)?\s*\}"
    )
    return {int(value, 16) for value in pattern.findall(text)}


def parse_function_definitions(text: str) -> tuple[set[str], set[int]]:
    names = set(re.findall(
        r"^void\s+(func_[0-9A-Fa-f]+(?:_cont_[0-9A-Fa-f]+)?)"
        r"\(CPUState\* cpu\)\s*\{",
        text,
        re.M,
    ))
    entries = {
        int(match.group(1), 16)
        for name in names
        if (match := re.fullmatch(r"func_([0-9A-Fa-f]+)", name))
    }
    return names, entries


def parse_ranges(path: Path) -> tuple[set[int], list[tuple[int, int]]]:
    entries: set[int] = set()
    ranges: list[tuple[int, int]] = []
    if not path.is_file():
        return entries, ranges
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = raw.split()
        if len(parts) == 2 and parts[0] == "F":
            entries.add(int(parts[1], 16))
        elif len(parts) == 3 and parts[0] == "R":
            lo = int(parts[1], 16)
            ranges.append((lo, lo + int(parts[2], 16)))
    return entries, ranges


def main() -> int:
    cfg = audit_config.from_argv(sys.argv)
    json_out = option_value(sys.argv, "--json-out")

    print(f"# codegen audit: {cfg.name}")
    print(f"  full_c     = {cfg.full_c}")
    print(f"  dispatch_c = {cfg.dispatch_c}")
    print()

    if not cfg.full_c.exists():
        print(f"ERROR: full_c not found: {cfg.full_c}", file=sys.stderr)
        return 2
    if not cfg.dispatch_c.exists():
        print(f"ERROR: dispatch_c not found: {cfg.dispatch_c}", file=sys.stderr)
        return 2

    full_c = cfg.full_c.read_text(encoding="utf-8", errors="replace")
    dispatch_c = cfg.dispatch_c.read_text(encoding="utf-8", errors="replace")
    expected_misses = EXPECTED_TAIL_CALL_MISSES.get(cfg.name, set())

    dynamic_path = Path(str(cfg.full_c).replace("_full.c", "_dynamic_targets.json"))
    dynamic_targets_raw: set[int] = set()
    dynamic_manifest_error = ""
    if dynamic_path.is_file():
        try:
            payload = json.loads(dynamic_path.read_text(encoding="utf-8-sig"))
            dynamic_targets_raw = {int(item["target"], 16) for item in payload.get("targets", [])}
        except Exception as exc:
            dynamic_manifest_error = str(exc)
    dynamic_targets = {cfg.normalize_addr(value) for value in dynamic_targets_raw}

    definition_names, definition_entries_raw = parse_function_definitions(full_c)
    table_raw = parse_dispatch_entries(dispatch_c)
    table_set = {cfg.normalize_addr(value) for value in table_raw}
    definition_entries = {
        cfg.normalize_addr(value) for value in definition_entries_raw
    }

    # ---- 0. no fallback/stub/unsupported output ----
    unknown_lines = [
        (number, line.strip())
        for number, line in enumerate(full_c.splitlines(), 1)
        if "psx_unknown_dispatch(" in line
        and not line.lstrip().startswith("extern ")
    ]
    unsupported_lines = [
        (number, line.strip())
        for number, line in enumerate(full_c.splitlines(), 1)
        if "TODO: opcode" in line
        or "TODO: SPECIAL" in line
        or "UNSUPPORTED INSTRUCTION" in line
    ]
    print(
        f"[0] generated fallback audit: unknown_dispatch_calls={len(unknown_lines)} "
        f"unsupported_markers={len(unsupported_lines)}"
    )
    for number, line in (unknown_lines + unsupported_lines)[:20]:
        print(f"      line {number}: {line}")

    # ---- 1. legacy predicate-init regression ----
    buggy_decls = re.findall(r"int psx_taken_[0-9A-F]+ = \([^)]*\);", full_c)
    zero_decls = re.findall(r"int psx_taken_[0-9A-F]+ = 0;", full_c)
    assignments = re.findall(r"^\s+psx_taken_[0-9A-F]+ = \(", full_c, re.M)
    print(
        f"\n[1] predicate decl audit: buggy={len(buggy_decls)} "
        f"zero_init={len(zero_decls)} assigns={len(assignments)}"
    )

    # ---- 2. dispatch table and literal dispatch targets ----
    print(f"\n[2] dispatch table size: {len(table_set)} normalized entries")
    missing_definition_dispatch = sorted(definition_entries - table_set)
    print(
        "    compiled function entries missing from dispatch: "
        f"{len(missing_definition_dispatch)}"
    )
    for value in missing_definition_dispatch[:25]:
        print(f"      0x{value:08X}")

    literal_targets = re.findall(
        r"(?:psx_dispatch|call_by_address)\(cpu,\s*0x([0-9A-Fa-f]+)u?\)",
        full_c,
    )
    literal_set = {cfg.normalize_addr(int(value, 16)) for value in literal_targets}
    missing_literal_raw = {
        int(value, 16) for value in literal_targets
        if cfg.normalize_addr(int(value, 16)) not in table_set
    }
    external_literal = sorted({
        cfg.normalize_addr(value) for value in missing_literal_raw
        if not cfg.is_in_code_region(value)
    })
    missing_literal = sorted({
        cfg.normalize_addr(value) for value in missing_literal_raw
        if cfg.is_in_code_region(value)
        and cfg.normalize_addr(value) not in dynamic_targets
    })
    dynamic_literal = sorted({
        cfg.normalize_addr(value) for value in missing_literal_raw
        if cfg.normalize_addr(value) in dynamic_targets
    })
    print(
        f"    literal dispatch targets: {len(literal_set)} unique; "
        f"missing_in_image={len(missing_literal)} "
        f"dynamic_runtime={len(dynamic_literal)} "
        f"external={len(external_literal)}"
    )
    for value in missing_literal[:25]:
        print(f"      0x{value:08X}")

    # ---- 3. direct generated C calls ----
    direct_calls = re.findall(
        r"\b(func_[0-9A-Fa-f]+(?:_cont_[0-9A-Fa-f]+)?)\(cpu\);",
        full_c,
    )
    missing_direct_defs = sorted(set(direct_calls) - definition_names)
    print(
        f"\n[3] direct func_X(cpu) calls: {len(direct_calls)} sites, "
        f"{len(set(direct_calls))} unique; missing definitions="
        f"{len(missing_direct_defs)}"
    )
    for name in missing_direct_defs[:25]:
        print(f"      {name}")

    indirect_sites = re.findall(
        r"(?:psx_dispatch|call_by_address)\(cpu,\s*cpu->gpr\[(\d+)\]\)",
        full_c,
    )
    print(f"    indirect dispatch sites: {len(indirect_sites)} total")
    print(f"    by register: {dict(Counter(indirect_sites).most_common(8))}")

    # ---- 4. CPS/tail-call targets ----
    tail_calls = re.findall(r"cpu->pc = 0x([0-9A-Fa-f]+)u;\s*return;", full_c)
    tail_raw_set = {int(value, 16) for value in tail_calls}
    tail_set = {cfg.normalize_addr(value) for value in tail_raw_set}
    tail_missing_raw = {
        value for value in tail_raw_set
        if cfg.normalize_addr(value) not in table_set
    }
    external_tail = sorted({
        cfg.normalize_addr(value) for value in tail_missing_raw
        if not cfg.is_in_code_region(value)
    })
    tail_missing = sorted({
        cfg.normalize_addr(value) for value in tail_missing_raw
        if cfg.is_in_code_region(value)
    })
    dynamic_tail = sorted(set(tail_missing) & dynamic_targets)
    unexpected_tail = sorted(set(tail_missing) - expected_misses - dynamic_targets)
    print(
        f"\n[4] tail-call targets: {len(tail_set)} unique; "
        f"missing_in_image={len(tail_missing)} "
        f"external={len(external_tail)} unexpected={len(unexpected_tail)}"
    )
    for value in tail_missing[:25]:
        tag = (" [manifested runtime-installed code]" if value in dynamic_targets else
               " [expected runtime-installed code]" if value in expected_misses else "")
        print(f"      0x{value:08X}{tag}")
    if external_tail:
        print("    external/runtime-loaded targets:")
        for value in external_tail[:25]:
            print(f"      0x{value:08X}")

    # ---- 5. code-range and data-range manifests ----
    ranges_path = cfg.full_c.with_suffix(".ranges")
    range_entries_raw, code_ranges_raw = parse_ranges(ranges_path)
    range_entries = {cfg.normalize_addr(value) for value in range_entries_raw}
    missing_range_entries = sorted(definition_entries - range_entries)
    extra_range_entries = sorted(range_entries - definition_entries)

    data_path = Path(str(cfg.full_c).replace("_full.c", "_data_ranges.json"))
    data_ranges_raw: list[tuple[int, int]] = []
    data_manifest_error = ""
    if data_path.is_file():
        try:
            payload = json.loads(data_path.read_text(encoding="utf-8-sig"))
            for item in payload.get("ranges", []):
                data_ranges_raw.append((int(item["start"], 16), int(item["end"], 16)))
        except Exception as exc:  # fail-loud in the summary below
            data_manifest_error = str(exc)

    code_data_overlap: list[tuple[int, int, int, int]] = []
    for code_lo, code_hi in code_ranges_raw:
        for data_lo, data_hi in data_ranges_raw:
            if code_lo < data_hi and data_lo < code_hi:
                code_data_overlap.append((code_lo, code_hi, data_lo, data_hi))
    dispatch_in_data = sorted({
        addr for addr in table_raw
        if any(lo <= addr < hi for lo, hi in data_ranges_raw)
    })
    dynamic_not_in_data = sorted({
        addr for addr in dynamic_targets_raw
        if not any(lo <= addr < hi for lo, hi in data_ranges_raw)
    })
    dynamic_in_dispatch = sorted({
        addr for addr in dynamic_targets_raw
        if cfg.normalize_addr(addr) in table_set
    })

    print(f"\n[5] range manifest: {ranges_path}")
    print(
        f"    function entries={len(range_entries)} "
        f"missing={len(missing_range_entries)} extra={len(extra_range_entries)}"
    )
    print(
        f"    data manifest={data_path if data_path.is_file() else 'absent'} "
        f"ranges={len(data_ranges_raw)} parse_error={data_manifest_error or 'none'}"
    )
    print(
        f"    code/data overlaps={len(code_data_overlap)} "
        f"dispatch entries in data={len(dispatch_in_data)}"
    )
    print(
        f"    dynamic manifest={dynamic_path if dynamic_path.is_file() else 'absent'} "
        f"targets={len(dynamic_targets)} parse_error={dynamic_manifest_error or 'none'} "
        f"outside_data={len(dynamic_not_in_data)} dispatched={len(dynamic_in_dispatch)}"
    )

    real_bugs = (
        len(unknown_lines)
        + len(unsupported_lines)
        + len(buggy_decls)
        + len(missing_definition_dispatch)
        + len(missing_literal)
        + len(missing_direct_defs)
        + len(unexpected_tail)
        + len(missing_range_entries)
        + len(extra_range_entries)
        + len(code_data_overlap)
        + len(dispatch_in_data)
        + (1 if data_manifest_error else 0)
        + (1 if dynamic_manifest_error else 0)
        + len(dynamic_not_in_data)
        + len(dynamic_in_dispatch)
        + (1 if not ranges_path.is_file() else 0)
    )

    status = "CLEAN" if real_bugs == 0 else "ISSUES FOUND"
    print(f"\n[summary] {cfg.name}")
    print(f"  generated fallback calls       : {len(unknown_lines)}")
    print(f"  unsupported markers            : {len(unsupported_lines)}")
    print(f"  missing dispatch/definitions   : {len(missing_definition_dispatch) + len(missing_direct_defs)}")
    print(f"  manifested dynamic targets    : {len(dynamic_targets)}")
    print(f"  unexpected tail-call misses    : {len(unexpected_tail)}")
    print(f"  range/data manifest findings   : {len(missing_range_entries) + len(extra_range_entries) + len(code_data_overlap) + len(dispatch_in_data) + (1 if data_manifest_error else 0)}")
    print(f"  total real-bug findings        : {real_bugs}")
    print(f"  STATUS                         : {status}")

    report = {
        "schema": 1,
        "name": cfg.name,
        "full_c": str(cfg.full_c),
        "dispatch_c": str(cfg.dispatch_c),
        "ranges": str(ranges_path),
        "data_ranges": str(data_path) if data_path.is_file() else None,
        "dynamic_targets_manifest": str(dynamic_path) if dynamic_path.is_file() else None,
        "function_definitions": len(definition_names),
        "dispatch_entries_normalized": len(table_set),
        "generated_fallback_calls": len(unknown_lines),
        "unsupported_markers": len(unsupported_lines),
        "missing_function_dispatch_entries": [f"0x{x:08X}" for x in missing_definition_dispatch],
        "missing_literal_dispatch_targets": [f"0x{x:08X}" for x in missing_literal],
        "dynamic_literal_dispatch_targets": [f"0x{x:08X}" for x in dynamic_literal],
        "external_literal_dispatch_targets": [f"0x{x:08X}" for x in external_literal],
        "missing_direct_function_definitions": missing_direct_defs,
        "unexpected_tail_call_misses": [f"0x{x:08X}" for x in unexpected_tail],
        "dynamic_tail_call_targets": [f"0x{x:08X}" for x in dynamic_tail],
        "external_tail_call_targets": [f"0x{x:08X}" for x in external_tail],
        "range_missing_entries": [f"0x{x:08X}" for x in missing_range_entries],
        "range_extra_entries": [f"0x{x:08X}" for x in extra_range_entries],
        "data_range_count": len(data_ranges_raw),
        "code_data_overlap_count": len(code_data_overlap),
        "dispatch_in_data": [f"0x{x:08X}" for x in dispatch_in_data],
        "dynamic_target_count": len(dynamic_targets),
        "dynamic_targets": [f"0x{x:08X}" for x in sorted(dynamic_targets)],
        "dynamic_targets_outside_data": [f"0x{x:08X}" for x in dynamic_not_in_data],
        "dynamic_targets_in_dispatch": [f"0x{x:08X}" for x in dynamic_in_dispatch],
        "real_bug_findings": real_bugs,
        "status": status,
    }
    if json_out:
        output = Path(json_out)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"  json report                    : {output}")

    return 0 if real_bugs == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
