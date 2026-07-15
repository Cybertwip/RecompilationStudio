# BIOS COP1 / `FUN_bfc02324` proof

Date: 2026-07-12

## Question

`psxrecomp-bios --emit-full` previously skipped BIOS function `0xBFC02324`
after encountering word `0x44801000` (`MTC1`) at `0xBFC0233C`, then emitted an
`psx_unknown_dispatch` body for a directly-called BIOS function. The question
was whether this range was data, host-software floating point, or executable
absent-coprocessor code.

## Source 1 — annotated BIOS disassembly

`docs/psx_bios_disasm.txt` does not cover this function. That is recorded as a
primary-reference gap, not filled by inference.

## Source 2 — Ghidra 11.3.2

Program: `SCPH1001.BIN`, base `0xBFC00000`.

- Ghidra defines `FUN_bfc02324`, body `0xBFC02324..0xBFC0258F`.
- Ghidra decompiles it as a decimal-string-to-double routine.
- Xrefs to `0xBFC02324`:
  - unconditional call at `0xBFC02598` in `FUN_bfc02590`;
  - data/table reference at `0xBFC043C8`.
- The function contains COP1 encodings plus two `LWC1` words and a `BC1F`.

Therefore the range is executable BIOS code, not scanner data.

## Source 3 — Beetle PSX oracle implementation

Official `libretro/beetle-psx-libretro`, commit
`004268513bb56655fff358b8caee88503a141776`, `mednafen/psx/cpu.c`:

- COP1/COP3 with the matching SR.CU bit clear raises Coprocessor Unusable
  (`ExcCode=11`) and writes Cause.CE from the coprocessor number.
- With CU1 set, non-branch COP1 operations are NOP-like.
- With CU1 set, the absent condition value is false: BC1F takes, BC1T does not.
- LWC1 performs alignment checking and an aligned read whose value is discarded.
- SWC1 performs alignment checking and no memory write.

This rules out host floating-point execution as unfaithful to retail PS1
hardware.

## Implementation and proof

The strict translator, decoder, discovery CFG, runtime interpreters, and shared
cycle/memory helpers now model those semantics. A clean BIOS regeneration gives:

- emitted functions: `1311`;
- skipped functions: `0` (no `skipped_functions.json` emitted);
- emitted instructions: `62024`;
- dispatch entries: `4439`;
- structural translator suite: `47/47` passing.

Generated code is not edited; the fix is entirely in framework source.
