# BIOS branding patch — stabilized implementation

Verified against canonical `SCPH1001.BIN` (SHA-256
`71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3`)
and the BIOS routines inspected in Ghidra 11.3.2 at `0xBFC00000`.

## Stable image conversion

Both selected branding images use nearest-neighbor (`Qt::FastTransformation`)
scaling. No image is divided into interleaved or adjoining texture pieces.

- Initial image: one legal 4-bit 120x90 TIM in the original SONY slot at ROM
  `0x5CC30`. This is the largest exact 4:3 image that fits that verified slot.
  The existing textured-quad object scales it to 640x480.
- Handover image: one 4-bit 200x40 TIM written identically to both stock
  PlayStation wordmark variants at `0x4EF60` and `0x4FF40`.
- The initial COMPUTER ENTERTAINMENT/TM assets and the handover TM asset are
  transparent.

Writing the same handover image to both wordmark variants removes the need to
modify the BIOS caller mode, region selector, or handover control flow.

## Fault removal

The rejected implementation rewrote caller arguments and the handover draw-
environment setup at:

- `0xBFC186E4`, `0xBFC186FC`;
- `0xBFC27434`, `0xBFC27438`;
- `0xBFC29BCC`, `0xBFC29BD0`, `0xBFC29BD8`.

Those changes caused repeated transitions and a BIOS fault before game entry.
The stabilized patch leaves every instruction above byte-identical to the
canonical BIOS. Only the two handover TIM assets are replaced, so whichever
stock regional variant the BIOS selects resolves to the requested image.

The safe geometry edits remain limited to centering the native 200x38 handover
object at `(320,240)`. The rejected 360x100 expansion sampled outside its
200x40 texture and caused malformed output and neighboring-VRAM color bars.

## Diamond removal and first image

The earlier SCE diamond builder and its three primitive submissions are skipped,
and its gray clears are changed to black. The later model-driven diamond keeps
its render loop but receives a zero model count at `0xBFC29B34`.

The full-frame initial image uses the existing double-buffered textured object:
its current-buffer object is sized to 640x480 and the unused remaining text-
object positioning is skipped.

## Frontend presentation gate

The patcher writes marker `PSXBRGATE001` into verified zero padding at ROM
`0x5E2D0`. The runtime decodes a color signature from the replacement initial
TIM, presents black while hidden BIOS setup frames execute, and begins normal
SDL/OpenGL/Vulkan presentation only when the displayed framebuffer matches the
selected first image. Unmarked BIOS images do not enter this path.

When Studio's **Skip BIOS intro and boot directly to the game** option is used,
the gate releases at compiled game entry because the branded BIOS frame is
intentionally never rendered.

## Direct-to-game Studio option

Studio emits the existing boot-skip configuration when selected:

```toml
[runtime]
bios_hle = false
bios_hle_keep_intro = false
fast_boot = true
```

This keeps the recompiled BIOS linked and uses the runtime's established shell-
skip/direct-disc-boot path without enabling HLE kernel-service replacements.
The normal default remains the real recompiled BIOS intro.

## Verification

- `PSXBiosBrandingPatcherTests` checks the TIMs, nearest-neighbor output slots,
  marker, safe instruction edits, stock handover control-flow instructions, and
  both identical handover variants.
- `PSXRecompStudioTests` checks normal and direct-to-game TOML emission.
- `psxrecomp-bios` emits the stabilized patched BIOS with zero skipped functions
  and no unsupported instructions.
- `bios_branding_patch.json` records every changed TIM/instruction and all source
  and output hashes.
