//! GBA machine model.
//!
//! The interpreter executes the `armv4t::Instr` model directly. It serves
//! three roles in the project (see docs/architecture.md):
//! 1. the executable specification of ARM7TDMI semantics,
//! 2. the differential-testing oracle for the recompiler,
//! 3. the runtime fallback tier for RAM-resident/self-modifying code.
//!
//! Correctness over speed: this is the reference implementation. The
//! recompiler's output is what has to be fast.

// MVII: bare metal, so no `std` and no allocator of our own — `alloc` routes
// to the Virtua libc's malloc through the global allocator the `gba-mvii`
// crate installs. `nostd` must come first: it is `#[macro_use]` so that the
// modules below still have `eprintln!`, and its prelude carries the `alloc`
// containers `#![no_std]` takes out of scope.
#![no_std]
// The `use crate::nostd::*;` at the top of each module is applied uniformly so
// that no file needs a bespoke import list; in the ones that never allocate it
// is dead, which is fine and not worth a per-file exception.
#![allow(unused_imports)]

extern crate alloc;

#[macro_use]
mod nostd;

pub mod apu;
pub mod backup;
pub mod bus;
pub mod capi;
pub mod cpu;
pub mod engine;
pub mod exec;
pub mod gax;
pub mod hle;
pub mod hostclock;
pub mod machine;
pub mod mem;
pub mod mp2k;
pub mod ppu;
pub mod rdrv;
pub mod rtc;
pub mod shadow;

pub use bus::Bus;
pub use cpu::{Cpu, Mode};
pub use machine::{instr_cost as machine_instr_cost, is_self_loop, Machine, StepEvent};
pub use mem::MemMap;
