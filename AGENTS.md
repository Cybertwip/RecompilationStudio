# PSXRecomp v4 — Rules

This file is the constitution for v4. Read it at the start of every
session before doing any work.

---

## -1. Local verification policy — Beetle is not required

Beetle is **not** part of mandatory session bring-up or ordinary debugging.
Do not restore, build, launch, or require `psx-beetle`, its static library, or an
embedded oracle unless the user explicitly requests Beetle for that task. The
required evidence path is: BIOS disassembly, Ghidra MCP, and the native
runtime's TCP debug harness.

Ghidra bring-up is deterministic and documented in
`docs/internal/GHIDRA_MCP_BRINGUP.md`. Always probe and reuse ports `8080` and
`8081` before launching anything; never open a second MCP-owning CodeBrowser.

---

## 0. The architecture is locked

v4 implements **Architecture A**: static MIPS-to-C recompilation of
`bios/SCPH1001.BIN`, producing native C that links into the runtime as
real compiled functions.

There is **no MIPS interpreter** in v4 for the BIOS path. Not as a
fallback. Not as a "temporary" measure. Not for "code we couldn't
recompile yet". If a BIOS function cannot be recompiled, the recompiler
is wrong and must be fixed. The interpreter does not exist. Do not
write one.

There is **no HLE BIOS layer** in v4. No `bios.c` with case branches
intercepting A0/B0/C0 vectors. No C reimplementations of `OpenEvent` or
`StartCard` or `alloc_kernel_memory`. The BIOS IS the recompiled C
output of `SCPH1001.BIN`. If a BIOS routine misbehaves, the answer is
to fix the recompiler or fix the hardware simulation it touches via
MMIO — never to write a C "shim" that produces the answer the BIOS
would have produced.

There are **no stubs**. A function is either fully implemented or it
aborts with a fatal error. `return 0;`, `return 1;`, `cpu->v0 = 1;
return;` are all stubs. `// TODO`, `// FIXME`, `// for now` are all
stubs. Hand-delivering an event because the chain handler isn't
installed is a stub wearing a costume and is the worst kind because it
hides the missing integration.

If you find yourself wanting to violate any of the above three
paragraphs, **stop and re-read `docs/internal/PLAN.md`**. Every prior attempt failed by
violating exactly these rules under pressure.

---

## 1. The BIOS is recompilation target #1, the game is target #2

Phase 1-3 of PLAN.md exist to get the BIOS recompiled and booting on
its own. The BIOS must reach the Sony logo and the BIOS shell, running
entirely as native C, before any game work begins. There is no path
that loads a game EXE before the BIOS is fully working in v4. **Do not
load a game ISO. Do not load a game EXE.** Tomba does not exist in v4
until Phase 5.

If you find yourself needing to load a game to "test something",
whatever you're testing belongs to a phase that hasn't started yet.

---

## 2. Required sources of truth, in priority order

Truth comes from these required sources, in this order:

1. **BIOS disassembly** at `docs/psx_bios_disasm.txt` for what the BIOS
   code is supposed to do. Check this first.
2. **Ghidra MCP** for exact bytes, instructions, function boundaries, and
   decompilation. Use `tools/ghidra_mcp_bringup.py` and
   `tools/ghidra_mcp_client.py`; the full runbook is
   `docs/internal/GHIDRA_MCP_BRINGUP.md`.
3. **Native runtime TCP proof** for live behavior. Production exports strip the
   server, so use `tools/build_diagnostic_export.py`, then query with
   `tools/runtime_batch.py` and focused audit tools such as
   `tools/thread_sr_audit.py`.

Use all required sources applicable to the question. Do not guess or say
"probably". If the disassembly, Ghidra, and native TCP evidence do not answer
the question, the answer is "I don't know yet" and the next action is to build
the missing diagnostic command/tool. Beetle is optional only when the user
explicitly requests it; it is never a bring-up gate.

---

## 3. No printf debugging. No log files. Ever.

If you need to inspect runtime state, **build a TCP debug server
command** for it. The v3 build accumulated 555 GB of `boot_trace*.log`
and `card_test*.log` files because previous sessions used `fprintf` for
"just this one thing". The rule is absolute: **no `fprintf(stderr, ...)`
in source code, ever, for any reason.**

When the v4 runtime is built (Phase 2+), it will have a TCP debug
server on a fresh port. All inspection goes through that.

---

## 4. Never modify generated code

The output of the recompiler — files in `recompiler/output/` or
`generated/SCPH1001_full.c` etc. — is a build artifact. If the
generated code is wrong, the fix is in the recompiler source
(`recompiler/src/code_generator.cpp` and friends), not in the
generated file.

This is the same rule as v3 had, and it stays.

---

## 5. Don't accept partial milestones

Phase completion requires the user-visible end state, not "I think it
should work now". Phase 3 is "Sony logo displays on screen". Not "the
recompiler emitted code that probably draws the logo". Not "the GPU
command stream looks right in the debug server". **The pixels appear
on screen, or the phase is not done.**

This was the v3 failure mode: declaring "memory card screen freeze
RESOLVED" when in fact the screen had been unlocked by hand-delivering
a fake event. The fake delivery was not progress, it was theater.

---

## 6. Session start checklist

At the start of every session, before any code change:

1. Read this file (`AGENTS.md`).
2. Read `docs/internal/PLAN.md` to confirm the active phase and milestone.
3. Verify `docs/psx_bios_disasm.txt` exists.
4. Read `docs/internal/GHIDRA_MCP_BRINGUP.md`, then run
   `python3 tools/ghidra_mcp_bringup.py status`. Probe/reuse the existing
   `8080` Ghidra endpoint and `8081` Python bridge before launching anything.
5. If Ghidra is not healthy, use the deterministic importer/bring-up tools. Ask
   the user only if that documented bring-up fails.
6. State out loud: "Architecture A is locked. No interpreter fallback. No
   stubs. BIOS first. Game never until Phase 5. Beetle is not required."

If any required item fails, do not modify code; surface and repair the failure
first.

---

## 7. Salvage from v3 — what's allowed and what's not

The recompiler in `recompiler/` was salvaged from v3 because the
core MIPS-to-C translator pieces (`basic_block.cpp`, `control_flow.cpp`,
`function_analysis.cpp`, `mips_decoder.cpp`, `code_generator.cpp`)
operate on raw MIPS bytes and have nothing wrong with them. They just
need a new entry point that ingests a flat ROM at `0xBFC00000` instead
of a `PS-X EXE`-headered file, plus extensions to `code_generator.cpp`
to handle COP0 kernel-mode instructions the BIOS uses.

**The runner from v3 was not salvaged.** Specifically:

- `bios.c` (1808 LOC HLE shims) — discarded
- `interpreter.c` (919 LOC MIPS interpreter) — discarded
- `events.c`, `threads.c` — discarded (recompiled BIOS manages its own EvCB/TCB)
- `bios_trace.c`, `func_logger.c` — discarded (interpreter-era helpers)
- `main_runner.cpp` — discarded (drove the interpreter)

The hardware simulation files from v3 (`memory.c`, `gpu.c`,
`gpu_sw_renderer.c`, `dma.c`, `interrupts.c`, `timers.c`, `sio.c`,
`memcard.c`, `cdrom.c`, `iso_reader.cpp`, `gte.cpp`, `spu.c`,
`debug_server.c`) are **eligible for salvage in Phase 2** when v4
needs them, but they will be copied in **one at a time**, audited for
HLE-state-leakage and stub patterns first, and only the parts that are
hardware simulation (not BIOS state simulation) are kept.

**Do not bulk-copy `psxrecomp/runner/src/` from v3.** Doing so will
re-import the disease.

---

## 8. Reference the right project for examples

PSXRecomp v4 is a sibling project to:

- **N64Recomp** (RT64 team) — proven static recompilation model for N64
- **SuperMarioWorldRecomp** (`F:/Projects/SuperMarioWorldRecomp/`) — sibling SNES recomp
- **SuperMarioWorldRecomp-oracle** (`F:/Projects/SuperMarioWorldRecomp-oracle/`)
- **NESRecomp** — referenced in v3's debug_server.c comments

When you need to know "how does a recomp project handle X?", read those
projects. **Do not** look at v1 (`F:/Projects/psxrecomp/`) or v2
(`F:/Projects/psxrecomp-v2/`) or v3 (`F:/Projects/psxrecomp-projects-v3/`)
for architectural guidance. They are reference for what failed, not
what worked.

---

## 9. Memory and prior session context

Auto-memory continues to work across sessions. Existing v3-era memories
about printf rules, no-stubs, BIOS-first, DuckStation oracle, etc. all
still apply. New v4-specific memories should be tagged so future
sessions can tell them apart from v3 memories. The most important new
memory is: **"v3 failed because it was an interpreter+HLE emulator
masquerading as a recompiler. v4 fixes this by ACTUALLY recompiling
the BIOS."**

---

## 10. No speculative progress

If a step involves:

- indirect jumps
- relocation
- hardware interaction

You MUST produce:

- manifest
- proof artifact

Code without proof is invalid.

---

## 11. First milestone is absolute

Before any Phase 2 work:

- FIRST_MILESTONE.md must be complete
- boot_slice must compile
- all instructions must be supported

No exceptions.

---

## 12. Relocation is mandatory before full BIOS

Do NOT attempt full BIOS recompilation until:

- BOOT_RELOCATION_PLAN.md is implemented
- address_aliases.json exists
- duplicate code is impossible

---

## 13. No large-step execution

You may NOT:

- "recompile the full BIOS"
- "walk the entire ROM"

Until:

- function discovery pipeline exists
- manifest output is verified

---

## 14. Unknown is acceptable. Guessing is not.

If something is unknown:

→ STOP  
→ produce artifact showing unknown  

Do NOT guess behavior.

---

## 15. Broken tooling is never acceptable. Fix it when identified.

If a tool, command, or verification mechanism fails or returns
unexpected results:

→ **Fix the tool, immediately, the moment you identify the breakage.**
   Diagnose why it failed and repair it before continuing the
   investigation that surfaced it.
→ Do NOT route around it with indirect evidence.
→ Do NOT infer correctness from two broken implementations agreeing.
→ Do NOT log the breakage as a "caveat to live with" or carry it forward
   in handoffs as a known limitation. A known-broken tool is a debt that
   compounds: every later session pays interest in the form of
   reconstructed-from-fragments evidence and shaky conclusions.

"The screenshot command returns black" is not a reason to skip visual
verification. It is a reason to fix the screenshot command.

"Both the native runtime and interpreter show the same wrong value"
does not make the value correct. It means both have the same bug.

"The packaged app has no TCP listener" is not permission to inspect console
output instead. Rebuild the exact export with `PSX_DEBUG_TOOLS=ON` using
`tools/build_diagnostic_export.py`.

If you cannot fix the tool, **ask the user** what they observe. Never declare a
result correct without direct disassembly/Ghidra/runtime proof.

---

## 16. Native runtime and Ghidra use deterministic, reusable harnesses

The required live process is **`psx-runtime`**, with the JSON-over-newline TCP
debug server (normally port `4370`; use a fresh port for diagnostic exports).
All runtime inspection goes through this protocol. Use
`tools/runtime_batch.py` for arbitrary commands and add focused audit tools when
a question needs correlation across rings.

Production Studio Release exports set `PSX_DEBUG_TOOLS=OFF`. They are not valid
diagnostic targets because `main` does not call `debug_server_init`. Rebuild the
exact packaged inputs without modifying the app:

```bash
python3 tools/build_diagnostic_export.py \
  --app '/path/Game.app' \
  --workspace /private/tmp/psxrecomp-game-debug \
  --debug-port 4470
```

Ghidra uses one CodeBrowser/plugin endpoint on `8080` and one reusable Python
bridge on `8081`. Always run the status probe first. If `8081` is already open
while Ghidra is closed, reuse that bridge after restarting Ghidra. Never send a
second GhidraGo request while `8080` is healthy; switching projects requires
closing/restarting Ghidra so only one MCP-owning CodeBrowser exists.

Beetle is not required, is not a session gate, and must not be restored, built,
or launched unless the user explicitly requests it.

---

## 17. Phase 5 gate — fix hardware stubs before loading a game

`STUBS_TO_FIX.md` lists every known stub in the runtime. Before any
Phase 5 work (loading Tomba or any game EXE), every stub marked
"Phase 5+" in that file **must be implemented and verified through the documented hardware behavior, Ghidra, and native TCP proof**:

- **S3 — MDEC decoder** (FMV playback)
- **S4 — SPU audio synthesis** (sound output)
- **S5 — DMA channels 0/1/3/4** (MDEC, CDROM, SPU data pipes)

These cannot be tested until disc data flows, but they cannot be
skipped either. The first task of Phase 5 is to implement them, not
to load the game and see what breaks. Loading the game with known
stubs is how v3 ended up with 1808 lines of shims.

---

## 18. Self-modifying / install-at-runtime RAM is interpreted, not HLE'd

The PSX BIOS dynamically writes 4-instruction dispatch stubs into kernel
RAM (e.g. RAM 0xCF0 for the SIO data-byte handler) and then transfers
control to those addresses. The static recompiler CANNOT see those
instructions, because they don't exist at compile time — only the
program's intent to install them does.

**The correct answer is to interpret, not to HLE.** A small MIPS
interpreter in the runtime tracks writes into the kernel-RAM code region,
marks affected pages "dirty", and runs any `psx_dispatch` whose target
falls in a dirty page through the interpreter. The interpreter executes
the program's own instructions on the CPU register state. After the basic
block, control returns to static-recompiled C.

**This is not HLE.** HLE means "the program would have produced result X,
so we synthesize X ourselves and skip the program's code." This rule is
the opposite: we run the program's code, exactly as the BIOS author wrote
it. The only difference from a pure static recompile is the *source* of
the instructions (RAM-written-at-runtime vs ROM-at-compile-time).

**This rule does NOT relax Rule 0.** The interpreter is not a fallback for
code the recompiler failed to translate. If a function exists in ROM at
recompile time, it MUST be statically recompiled. The interpreter only
runs against PCs in pages that have been written to since boot — i.e.,
code that was put there at runtime by the program.

Mature static-recompilation projects (N64Recomp, mednafen-PSX's dynarec)
all handle install-at-runtime code this way. PSXRecomp v4 follows suit.

Implementation lives in `runtime/src/dirty_ram_interp.c` (or similar). It
is intentionally small (~300 LOC), modular, and isolated. It does NOT
expand into a general-purpose CPU emulator.
