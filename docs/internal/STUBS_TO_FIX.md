# Stubs to Fix

Status refreshed: 2026-07-12. The five entries from the original 2026-04-24
stub audit are resolved in the current tree. Hardware-accuracy gaps that still
produce real behavior rather than a no-op, crash, or fabricated result are
tracked separately in `docs/internal/ACCURACY_BURNDOWN.md` and the
`accuracy/axis5_*.md` reports.

## Open entries from the 2026-04-24 audit

None.

## Resolved

### S1 — GPU shaded-line interpolation

- **Resolved by:** `5a63372` (2026-04-24).
- `gp0_exec_shaded_line()` reads both endpoint colors and calls
  `gr_draw_shaded_line()` with C0 and C1.

### S2 — GPU polyline command handling

- **Resolved by:** `5a63372` (2026-04-24), with terminator correction in
  `3c67a59` (2026-07-02).
- GP0 0x48-0x4F and 0x58-0x5F use variable-length mono/shaded polyline state
  machines and terminate on the hardware masked sentinel pattern.

### S3 — MDEC decode and DMA channels 0/1

- **Resolved in:** `runtime/src/mdec.c` and `runtime/src/dma.c`.
- Command decode, quant/scale loading, RLE, IDCT, YUV conversion, output packing,
  status, and DMA input/output are implemented.
- **Proof:** `tools/mdec_pin/FINDING.md` records a byte-exact corrected end-to-end
  pin and a live VRAM comparison against the independent Beetle process.

### S4 — SPU audio synthesis

- **Resolved in:** `runtime/src/spu.c` and the runtime audio path.
- The runtime synthesizes and mixes 24 ADPCM voices with pitch, ADSR, loop/end
  handling, key-on/key-off, CD input, and host audio output.
- **Proof:** the measured Beetle A/B audio campaign merged in `54df1ec`; its
  debug rings and offline comparison tools remain under `tools/audio_*.py`.
- Reverb, noise, sweep, pitch modulation, capture buffers, and exact SPU IRQ
  behavior remain accuracy work, not a no-audio stub; see `accuracy/axis5_spu.md`.

### S5 — DMA channels required for game support

- **Resolved in:** `runtime/src/dma.c`.
- Channel 0 (RAM to MDEC), channel 1 (MDEC to RAM), channel 3 (CD-ROM to RAM),
  and channel 4 (RAM/SPU transfers) have concrete handlers and completion paths.
- Their active game paths are exercised by CD/MDEC FMV playback and SPU sample
  transfer; device timing refinements remain tracked by the accuracy plan.

### Earlier resolved item — DMA channel 2 GPU to RAM

- **Resolved by:** `4054dc1` (2026-04-24).
- GPU-to-RAM DMA reads GPUREAD instead of writing zeros.
