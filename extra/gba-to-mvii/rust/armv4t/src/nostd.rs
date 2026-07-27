//! The platform floor this crate stands on when there is no operating system.
//!
//! The decoder is pure computation — it touches no files, no clock and no
//! environment. Its only tie to `std` is the disassembler's string building,
//! which `alloc` covers, so this shim is just the allocating half of the
//! prelude that `#![no_std]` takes away. A glob import at the top of each
//! source file brings it back, leaving the upstream text (extra/gba-rust,
//! crates/armv4t) otherwise untouched.

#![allow(dead_code, unused_imports)]

pub use alloc::borrow::ToOwned;
pub use alloc::boxed::Box;
pub use alloc::format;
pub use alloc::string::{String, ToString};
pub use alloc::vec;
pub use alloc::vec::Vec;
