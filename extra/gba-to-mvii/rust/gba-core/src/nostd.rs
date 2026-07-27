//! The platform floor this crate stands on when there is no operating system.
//!
//! Upstream (extra/gba-rust) targets a hosted platform and uses `std` for four
//! things and four only: the allocating containers, environment variables
//! behind the diagnostic traces, `OnceLock` caching those same traces, and the
//! `f64` methods that live in libm. MVII's Virtua userland has an allocator, a
//! C library and a math library, but no `std` — so this module supplies those
//! four, and a glob import at the top of every source file pulls them in.
//!
//! The point is that the emulation modules themselves are unmodified. A
//! divergence between this runtime and the reference implementation would be a
//! bug we could not reason about, so the only edits to mem.rs, ppu.rs, exec.rs
//! and the rest are the one-line import and (in hostclock.rs) the host-clock
//! call. Everything else compiles from the upstream text.
//!
//! Two names deliberately resolve to something different than they would on a
//! desktop build:
//!
//!   `std::env::var*`  always reports "not set". The traces it gates are
//!                     developer plumbing for the desktop launcher; on the
//!                     handheld they are dead code, which is what lets LLVM
//!                     drop the formatting machinery behind them.
//!   `std::sync`       single-threaded. A Virtua process is one thread
//!                     cooperatively scheduled by the kernel, so `OnceLock`
//!                     needs no blocking path — see the note on its `Sync`.

#![allow(dead_code, unused_imports)]

// ── the alloc prelude ──────────────────────────────────────────────────────
// `no_std` keeps the core prelude, which has no heap types. These are the
// names the emulation modules expect to find without qualification.

pub use alloc::borrow::ToOwned;
pub use alloc::boxed::Box;
pub use alloc::format;
pub use alloc::string::{String, ToString};
pub use alloc::vec;
pub use alloc::vec::Vec;

pub use self::shim::float::FloatOps;

/// Aliased over the `std` path by the glob import, so `std::mem::…`,
/// `std::env::…`, `std::sync::…` in the upstream text keep resolving.
pub use self::shim as std;

pub mod shim {
    pub use core::{cmp, fmt, hash, iter, marker, mem, num, ops, ptr, slice, str};

    /// `std::collections`, restricted to what `alloc` can offer without a
    /// hasher. Only `VecDeque` is actually used (the Direct Sound FIFOs).
    pub mod collections {
        pub use alloc::collections::{btree_map, BTreeMap, BTreeSet, VecDeque};
    }

    /// `std::f32` / `std::f64` exist here only for `consts` — the BIOS
    /// ArcTan/ArcTan2 HLE reaches for `PI`. `f64::MAX` and `f64::NAN` are
    /// associated constants on the primitive and need nothing from us.
    pub mod f32 {
        pub use core::f32::consts;
    }
    pub mod f64 {
        pub use core::f64::consts;
    }

    /// No environment on a Virtua process: the kernel hands a guest argv and
    /// nothing else. Every caller of these is a diagnostic trace that stays
    /// switched off, so they answer "not set" rather than pretending.
    pub mod env {
        use alloc::string::String;

        /// Stands in for `std::ffi::OsString`. Never constructed — `var_os`
        /// only ever returns `None` — but the call sites name the type
        /// through `.is_some()`, so it has to exist.
        pub struct OsString;

        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        pub enum VarError {
            NotPresent,
        }

        pub fn var_os(_name: &str) -> Option<OsString> {
            None
        }

        pub fn var(_name: &str) -> Result<String, VarError> {
            Err(VarError::NotPresent)
        }
    }

    pub mod sync {
        pub use super::once::OnceLock;
    }

    pub mod once {
        use core::cell::UnsafeCell;
        use core::sync::atomic::{AtomicU8, Ordering};

        const EMPTY: u8 = 0;
        const FULL: u8 = 1;

        /// `std::sync::OnceLock` without the blocking path.
        ///
        /// A Virtua process runs on exactly one thread — MVII schedules
        /// processes, not threads inside them — so the race `std`'s version
        /// guards against cannot occur here, and the initializer can run
        /// straight through. The atomic is kept because the type must be
        /// `Sync` to live in a `static`, and because it is free on ARM once
        /// the ordering is `Relaxed`.
        pub struct OnceLock<T> {
            state: AtomicU8,
            value: UnsafeCell<Option<T>>,
        }

        // SAFETY: single-threaded guest, as above. `T: Send + Sync` is still
        // required so a `&OnceLock<T>` cannot hand out a `&T` that would be
        // unsound to share even in principle.
        unsafe impl<T: Send + Sync> Sync for OnceLock<T> {}
        unsafe impl<T: Send> Send for OnceLock<T> {}

        impl<T> OnceLock<T> {
            pub const fn new() -> OnceLock<T> {
                OnceLock {
                    state: AtomicU8::new(EMPTY),
                    value: UnsafeCell::new(None),
                }
            }

            pub fn get(&self) -> Option<&T> {
                if self.state.load(Ordering::Relaxed) == FULL {
                    // SAFETY: FULL is only published after the write below.
                    unsafe { (*self.value.get()).as_ref() }
                } else {
                    None
                }
            }

            pub fn set(&self, value: T) -> Result<(), T> {
                if self.state.load(Ordering::Relaxed) == FULL {
                    return Err(value);
                }
                // SAFETY: no other borrow is live (single-threaded, and `get`
                // returns `None` while the state is EMPTY).
                unsafe { *self.value.get() = Some(value) };
                self.state.store(FULL, Ordering::Relaxed);
                Ok(())
            }

            pub fn get_or_init<F: FnOnce() -> T>(&self, init: F) -> &T {
                if self.state.load(Ordering::Relaxed) != FULL {
                    let value = init();
                    // SAFETY: as in `set`.
                    unsafe { *self.value.get() = Some(value) };
                    self.state.store(FULL, Ordering::Relaxed);
                }
                // SAFETY: FULL, so the option is `Some`.
                unsafe { (*self.value.get()).as_ref().unwrap() }
            }
        }

        impl<T> Default for OnceLock<T> {
            fn default() -> OnceLock<T> {
                OnceLock::new()
            }
        }
    }

    /// The `f32`/`f64` methods that libm owns.
    ///
    /// `core` has the ones the compiler can do in registers (`abs`, `max`,
    /// `is_finite`, …) and leaves the transcendentals to the C math library.
    /// The Virtua sysroot ships one — llvm-libc's libm.a, which the link line
    /// already pulls in — so these forward to it rather than dragging in a
    /// second implementation.
    ///
    /// Where `core` does have an inherent method of the same name, Rust picks
    /// the inherent one; this trait only fills the holes.
    pub mod float {
        extern "C" {
            fn sqrt(x: f64) -> f64;
            fn floor(x: f64) -> f64;
            fn ceil(x: f64) -> f64;
            fn round(x: f64) -> f64;
            fn trunc(x: f64) -> f64;
            fn fabs(x: f64) -> f64;
            fn atan(x: f64) -> f64;
            fn atan2(y: f64, x: f64) -> f64;
            fn pow(x: f64, y: f64) -> f64;
            fn exp(x: f64) -> f64;
            fn log(x: f64) -> f64;

            fn sqrtf(x: f32) -> f32;
            fn floorf(x: f32) -> f32;
            fn ceilf(x: f32) -> f32;
            fn roundf(x: f32) -> f32;
            fn truncf(x: f32) -> f32;
            fn fabsf(x: f32) -> f32;
            fn atanf(x: f32) -> f32;
            fn atan2f(y: f32, x: f32) -> f32;
            fn powf(x: f32, y: f32) -> f32;
            fn expf(x: f32) -> f32;
            fn logf(x: f32) -> f32;
        }

        pub trait FloatOps: Sized + Copy {
            fn sqrt(self) -> Self;
            fn floor(self) -> Self;
            fn ceil(self) -> Self;
            fn round(self) -> Self;
            fn trunc(self) -> Self;
            fn abs(self) -> Self;
            fn atan(self) -> Self;
            fn atan2(self, other: Self) -> Self;
            fn powf(self, other: Self) -> Self;
            fn powi(self, n: i32) -> Self;
            fn exp(self) -> Self;
            fn ln(self) -> Self;
        }

        impl FloatOps for f64 {
            fn sqrt(self) -> f64 {
                unsafe { sqrt(self) }
            }
            fn floor(self) -> f64 {
                unsafe { floor(self) }
            }
            fn ceil(self) -> f64 {
                unsafe { ceil(self) }
            }
            fn round(self) -> f64 {
                unsafe { round(self) }
            }
            fn trunc(self) -> f64 {
                unsafe { trunc(self) }
            }
            fn abs(self) -> f64 {
                unsafe { fabs(self) }
            }
            fn atan(self) -> f64 {
                unsafe { atan(self) }
            }
            fn atan2(self, other: f64) -> f64 {
                unsafe { atan2(self, other) }
            }
            fn powf(self, other: f64) -> f64 {
                unsafe { pow(self, other) }
            }
            fn powi(self, n: i32) -> f64 {
                unsafe { pow(self, n as f64) }
            }
            fn exp(self) -> f64 {
                unsafe { exp(self) }
            }
            fn ln(self) -> f64 {
                unsafe { log(self) }
            }
        }

        impl FloatOps for f32 {
            fn sqrt(self) -> f32 {
                unsafe { sqrtf(self) }
            }
            fn floor(self) -> f32 {
                unsafe { floorf(self) }
            }
            fn ceil(self) -> f32 {
                unsafe { ceilf(self) }
            }
            fn round(self) -> f32 {
                unsafe { roundf(self) }
            }
            fn trunc(self) -> f32 {
                unsafe { truncf(self) }
            }
            fn abs(self) -> f32 {
                unsafe { fabsf(self) }
            }
            fn atan(self) -> f32 {
                unsafe { atanf(self) }
            }
            fn atan2(self, other: f32) -> f32 {
                unsafe { atan2f(self, other) }
            }
            fn powf(self, other: f32) -> f32 {
                unsafe { powf(self, other) }
            }
            fn powi(self, n: i32) -> f32 {
                unsafe { powf(self, n as f32) }
            }
            fn exp(self) -> f32 {
                unsafe { expf(self) }
            }
            fn ln(self) -> f32 {
                unsafe { logf(self) }
            }
        }
    }
}

// ── diagnostics ────────────────────────────────────────────────────────────

extern "C" {
    /// Provided by the runtime (see rust/gba-mvii/src/lib.rs), which forwards
    /// to MVII's stderr. Every caller sits behind a trace flag that this build
    /// leaves off, so on the normal path this is never reached — which matters,
    /// because MVII's `_write` puts stderr on the serial console one byte at a
    /// time and a per-frame trace would be slower than the emulation itself.
    fn gba_mvii_diag_write(ptr: *const u8, len: usize);
}

struct DiagSink;

impl core::fmt::Write for DiagSink {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        // SAFETY: `s` is a valid UTF-8 slice; the callee only reads `len`
        // bytes and does not retain the pointer.
        unsafe { gba_mvii_diag_write(s.as_ptr(), s.len()) };
        Ok(())
    }
}

#[doc(hidden)]
pub fn diag_fmt(args: core::fmt::Arguments<'_>) {
    use core::fmt::Write;
    let _ = DiagSink.write_fmt(args);
    let _ = DiagSink.write_str("\n");
}

/// Stands in for `std`'s `eprintln!`.
macro_rules! eprintln {
    () => {
        $crate::nostd::diag_fmt(::core::format_args!(""))
    };
    ($($arg:tt)*) => {
        $crate::nostd::diag_fmt(::core::format_args!($($arg)*))
    };
}
