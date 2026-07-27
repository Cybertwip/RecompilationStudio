// bios_hle.h — standalone High-Level Emulation of the GBA BIOS.
//
// The upstream gba++ runtime treats the real BIOS as sacred: it recompiles the
// user's 16 KB dump and executes it (LLE), with HLE only as an opt-in overlay.
// That is not available here. MVII packages ship a ROM and nothing else — the
// generator emits a zero-filled gba_bios_zero.bin placeholder — so there are no
// BIOS bytes to execute and every BIOS entry point (reset at 0x00, SWI at 0x08,
// IRQ at 0x18) would vector into sixteen kilobytes of zeroes.
//
// So this port runs the equivalent of gba++'s BiosHleMode::Standalone: the
// three entry points are synthesized in host code and the BIOS region is never
// executed. The SWI bodies are ported verbatim from src/runtime/bios_hle.cpp
// (themselves ported from mGBA's src/gba/bios.c, MPL-2.0, © Jeffrey Pfau — see
// gba++/THIRD_PARTY_ATTRIBUTION.md); the IRQ entry/epilogue are ported from the
// `runtime_irq_standalone_begin_async` / `runtime_hle_irq_epilogue` pair in
// src/armv4t/runtime_arm.cpp.
//
// The one structural change is the register file. Upstream drives the
// recompiler's ArmCpuState (a flat `uint32_t cpsr` plus ARM_BANK_* slots) via
// the globals g_cpu / bus_read_u32 / bus_write_u32; here the same code drives
// the interpreter's armv4t::CPUState (a bitfield CPSR plus armv4t::Bank_*) and
// an armv4t::Bus reference, both bound once by bios_hle_bind().

#pragma once

#include <cstdint>

namespace armv4t { struct CPUState; struct Bus; }

namespace gbamvii {

// The address the HLE IRQ dispatcher parks in LR before calling the game's
// handler. It is inside the (unexecutable) BIOS region on purpose: the value
// can never collide with real code, and the run loop recognises it as "the
// game's IRQ handler just returned" without needing a host call stack.
inline constexpr uint32_t kHleIrqReturn = 0x00000138u;

// Bind the CPU/bus the HLE operates on. Must be called before anything else.
void bios_hle_bind(armv4t::CPUState* cpu, armv4t::Bus* bus);

// Synthesize the machine state the real BIOS leaves at cart handoff and jump
// to `cart_entry` (normally 0x08000000). The boot logo and chime never run.
void bios_hle_boot_skip(uint32_t cart_entry);

// Service SWI `swi` (the comment field, 0x00..0x1F). Returns the cycle cost.
// Call with R15 already advanced past the SWI instruction: SWI_INTR_WAIT and
// SWI_VBLANK_INTR_WAIT rewind R15 by one instruction width to re-execute
// themselves after the halt, which is how the BIOS wait loop is modelled.
uint32_t bios_hle_swi(uint32_t swi);

// Vector an IRQ the way the BIOS dispatcher at 0x18 does: push {r0-r3,r12,lr}
// on the IRQ stack, pass 0x04000000 in r0, LR = kHleIrqReturn, and jump to the
// user handler pointer at 0x03007FFC. No-op (returns false) when the game has
// not installed a handler yet, in which case the caller should leave the CPU
// alone — with IME/IE unset that cannot happen, but a game that enables IME
// before writing 0x03007FFC would otherwise execute address 0.
bool bios_hle_irq_enter(uint32_t return_address);

// Complete an HLE IRQ return when the game's handler branches to LR. Returns
// false if the CPU is not parked at kHleIrqReturn in IRQ mode, so the run loop
// can call it unconditionally once per instruction.
bool bios_hle_irq_epilogue();

}  // namespace gbamvii
