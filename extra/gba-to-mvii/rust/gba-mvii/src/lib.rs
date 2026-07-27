//! The seam between the Rust emulation core and the MVII half of the runtime.
//!
//! Everything that is specific to running without an operating system lives
//! here, so that `gba-core` and `armv4t` stay the reference implementation and
//! nothing else: the allocator, the panic handler, and a C ABI narrow enough
//! that the C++ side never needs to know a Rust type.
//!
//! Two shapes in that ABI are deliberate rather than convenient:
//!
//! * **The ROM buffer is allocated here and filled there.** A 16 MB cartridge
//!   read into a C++ buffer and then copied into a Rust `Vec` peaks at 32 MB,
//!   and an MVII guest gets a 32 MB heap. `gba_mvii_rom_alloc` hands out the
//!   `Vec`'s own storage; `gba_mvii_create` takes it back. One buffer, ever.
//!
//! * **Execution is stepped, not framed.** `gba_mvii_run_steps` runs a bounded
//!   number of instructions and returns; the caller yields to MVII between
//!   calls. A `run_frame` entry point would tie the whole system's
//!   responsiveness to the emulated frame rate, which on a Cortex-A7
//!   interpreting an ARM7TDMI is precisely the number nobody can promise. The
//!   kernel cuts its scheduler round at 120 Hz and expects to be asked often.

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};

use gba_core::backup::BackupKind;
use gba_core::machine::Machine;
use gba_core::mem::AUDIO_RATE_HZ;

// ── the bare-metal floor ───────────────────────────────────────────────────

extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn aligned_alloc(alignment: usize, size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
    fn abort() -> !;

    /// Implemented on the MVII side (src/mvii/mvii_platform.cpp): writes to
    /// the app log. Declared identically in gba-core's nostd.rs, which routes
    /// `eprintln!` through it.
    fn gba_mvii_diag_write(ptr: *const u8, len: usize);
}

/// MVII's libc allocator, wearing Rust's interface.
///
/// `malloc` returns storage aligned for any fundamental type — 8 bytes on ARM
/// EABI — which covers every allocation these crates make. Anything stricter
/// goes to `aligned_alloc`, with the size rounded up to a multiple of the
/// alignment because C11 requires that of the caller and llvm-libc is entitled
/// to enforce it. Both are released with plain `free`.
struct LibcAlloc;

unsafe impl GlobalAlloc for LibcAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= 8 {
            malloc(layout.size())
        } else {
            let align = layout.align();
            let size = (layout.size() + align - 1) & !(align - 1);
            aligned_alloc(align, size)
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr)
    }
}

#[global_allocator]
static ALLOC: LibcAlloc = LibcAlloc;

fn diag(args: core::fmt::Arguments<'_>) {
    use core::fmt::Write;

    struct Sink;
    impl core::fmt::Write for Sink {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            // SAFETY: valid UTF-8 slice; the callee reads `len` bytes and does
            // not retain the pointer.
            unsafe { gba_mvii_diag_write(s.as_ptr(), s.len()) };
            Ok(())
        }
    }
    let _ = Sink.write_fmt(args);
}

/// A Rust panic in here is an emulator bug, not a guest fault, and there is no
/// unwinder and nothing above us to catch it. Say where it happened — MVII
/// puts stderr on the app log *and* the serial console, so the message
/// survives the process — and stop.
#[panic_handler]
fn on_panic(info: &core::panic::PanicInfo<'_>) -> ! {
    diag(format_args!("gba: rust panic: {info}\n"));
    // SAFETY: `abort` is declared divergent and does not return.
    unsafe { abort() }
}

// ── C ABI ──────────────────────────────────────────────────────────────────

/// Opaque to C. Really a `Machine`, but the C++ side must never depend on that.
pub enum GbaMvii {}

/// Bumped whenever the ABI below changes shape. The C++ side checks it at
/// startup: a stale object file that links but disagrees about a struct is a
/// far worse failure than one that refuses to start.
pub const ABI_VERSION: u32 = 1;

#[inline]
unsafe fn machine<'a>(handle: *mut GbaMvii) -> &'a mut Machine {
    &mut *(handle as *mut Machine)
}

#[no_mangle]
pub extern "C" fn gba_mvii_abi_version() -> u32 {
    ABI_VERSION
}

#[no_mangle]
pub extern "C" fn gba_mvii_screen_width() -> u32 {
    gba_core::mem::VISIBLE_WIDTH as u32
}

#[no_mangle]
pub extern "C" fn gba_mvii_screen_height() -> u32 {
    gba_core::mem::VISIBLE_SCANLINES as u32
}

/// Sample rate of the interleaved stereo stream `gba_mvii_drain_audio` yields.
#[no_mangle]
pub extern "C" fn gba_mvii_audio_rate() -> u32 {
    AUDIO_RATE_HZ
}

// ---- cartridge ----

/// Storage for a `len`-byte ROM image, owned by Rust, to be filled by the
/// caller and handed straight back to `gba_mvii_create`. Null on failure.
///
/// The `Vec` from `vec![0u8; len]` has capacity exactly `len`, which is what
/// makes the round trip through `from_raw_parts` sound.
#[no_mangle]
pub extern "C" fn gba_mvii_rom_alloc(len: usize) -> *mut u8 {
    if len == 0 {
        return core::ptr::null_mut();
    }
    let mut rom: Vec<u8> = vec![0u8; len];
    let ptr = rom.as_mut_ptr();
    core::mem::forget(rom);
    ptr
}

/// Release a buffer from `gba_mvii_rom_alloc` that never reached `create`.
///
/// # Safety
/// `ptr`/`len` must be exactly what `gba_mvii_rom_alloc` returned and was
/// asked for, and must not have been passed to `gba_mvii_create`.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_rom_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() && len != 0 {
        drop(Vec::from_raw_parts(ptr, len, len));
    }
}

/// Build a machine around a ROM buffer, taking ownership of it.
///
/// # Safety
/// `rom`/`len` must come from `gba_mvii_rom_alloc`. Ownership transfers on
/// success *and* on failure — the buffer is never left for the caller to free.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_create(rom: *mut u8, len: usize) -> *mut GbaMvii {
    if rom.is_null() || len == 0 {
        return core::ptr::null_mut();
    }
    let rom = Vec::from_raw_parts(rom, len, len);
    let machine = Box::new(Machine::new(rom));
    Box::into_raw(machine) as *mut GbaMvii
}

/// As `gba_mvii_create`, but executes a real BIOS image instead of the HLE.
///
/// # Safety
/// As `gba_mvii_create`; `bios` must additionally point at `bios_len` readable
/// bytes for the duration of the call (they are copied).
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_create_with_bios(
    rom: *mut u8,
    len: usize,
    bios: *const u8,
    bios_len: usize,
) -> *mut GbaMvii {
    if rom.is_null() || len == 0 {
        return core::ptr::null_mut();
    }
    let rom = Vec::from_raw_parts(rom, len, len);
    if bios.is_null() || bios_len == 0 {
        return Box::into_raw(Box::new(Machine::new(rom))) as *mut GbaMvii;
    }
    let bios = core::slice::from_raw_parts(bios, bios_len);
    Box::into_raw(Box::new(Machine::new_with_bios(rom, bios))) as *mut GbaMvii
}

/// # Safety
/// `handle` must come from `gba_mvii_create*` and not have been destroyed.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_destroy(handle: *mut GbaMvii) {
    if !handle.is_null() {
        drop(Box::from_raw(handle as *mut Machine));
    }
}

/// The 12-character title from the cartridge header (0xA0), NUL-terminated.
/// Returns the number of bytes written, not counting the terminator.
///
/// # Safety
/// `out` must point at `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_rom_title(
    handle: *mut GbaMvii,
    out: *mut u8,
    cap: usize,
) -> usize {
    if handle.is_null() || out.is_null() || cap == 0 {
        return 0;
    }
    let rom = &machine(handle).bus.rom;
    let mut written = 0usize;
    for i in 0..12usize {
        if written + 1 >= cap {
            break;
        }
        let b = *rom.get(0xA0 + i).unwrap_or(&0);
        if b == 0 {
            break;
        }
        // Header titles are ASCII; anything else is a corrupt image, and a
        // control byte in the app log is worse than a question mark.
        let b = if (0x20..0x7F).contains(&b) { b } else { b'?' };
        *out.add(written) = b;
        written += 1;
    }
    *out.add(written) = 0;
    written
}

// ---- execution ----

/// KEYINPUT, active low: a set bit means released, 0x3FF means nothing held.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_set_keys(handle: *mut GbaMvii, keyinput: u16) {
    machine(handle).bus.keys = keyinput & 0x3FF;
}

/// Execute at most `steps` instructions, stopping early once the PPU finishes
/// a frame. Returns 1 if a completed frame is waiting.
///
/// Call this in a loop with a small `steps` and yield to MVII in between; see
/// the module note on why there is no `run_frame`.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_run_steps(handle: *mut GbaMvii, steps: u32) -> u32 {
    let m = machine(handle);
    let mut left = steps;
    while left != 0 && !m.bus.frame_ready {
        m.step();
        left -= 1;
    }
    m.bus.frame_ready as u32
}

/// Acknowledge the frame `gba_mvii_run_steps` reported, after presenting it.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_frame_consume(handle: *mut GbaMvii) {
    machine(handle).bus.frame_ready = false;
}

/// Frames the PPU has completed since reset.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_frames(handle: *mut GbaMvii) -> u64 {
    machine(handle).bus.frames
}

/// Master cycle counter — the emulated clock, for pacing.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_cycles(handle: *mut GbaMvii) -> u64 {
    machine(handle).bus.clock
}

// ---- video ----

/// The 240x160 frame, BGR555 (red in the low five bits), row-major, no
/// padding. Valid until the next `gba_mvii_run_steps`.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_framebuffer(handle: *mut GbaMvii) -> *const u16 {
    machine(handle).bus.framebuffer.as_ptr()
}

// ---- audio ----

/// Move up to `cap` interleaved stereo samples into `out` and drop them from
/// the queue. Returns how many were moved.
///
/// Call it every frame even when there is no DAC: the queue is unbounded, and
/// an emulator that never drains it leaks for as long as it runs.
///
/// # Safety
/// `handle` must be live; `out` must point at `cap` writable `i16`s.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_drain_audio(
    handle: *mut GbaMvii,
    out: *mut i16,
    cap: usize,
) -> usize {
    let buf = &mut machine(handle).bus.audio_buf;
    if out.is_null() || cap == 0 {
        buf.clear();
        return 0;
    }
    let n = buf.len().min(cap);
    core::ptr::copy_nonoverlapping(buf.as_ptr(), out, n);
    // Anything past `cap` is stale by the time the next frame is mixed; a
    // backlog would drift audio behind video forever rather than glitch once.
    buf.clear();
    n
}

// ---- backup medium ----

/// 0 none, 1 SRAM, 2 Flash 64K, 3 Flash 128K, 4 EEPROM.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_backup_kind(handle: *mut GbaMvii) -> u32 {
    match machine(handle).bus.backup {
        BackupKind::None => 0,
        BackupKind::Sram => 1,
        BackupKind::Flash64 => 2,
        BackupKind::Flash128 => 3,
        BackupKind::Eeprom => 4,
    }
}

/// Whichever array the cartridge actually saves into. Each branch re-derives
/// the machine reference from the raw handle rather than threading one borrow
/// through, because a single `&mut` returned from one arm and reused in the
/// next is the conditional-reborrow shape NLL still rejects.
///
/// # Safety
/// `handle` must be live, and the result must not outlive it.
unsafe fn save_bytes<'a>(handle: *mut GbaMvii) -> &'a mut [u8] {
    if machine(handle).bus.flash.is_some() {
        return machine(handle).bus.flash.as_mut().unwrap().data.as_mut_slice();
    }
    if machine(handle).bus.eeprom.is_some() {
        return machine(handle)
            .bus
            .eeprom
            .as_mut()
            .unwrap()
            .data
            .as_mut_slice();
    }
    if machine(handle).bus.backup == BackupKind::Sram {
        return machine(handle).bus.sram.as_mut_slice();
    }
    // No backup medium: a well-formed empty slice, not a null one.
    core::slice::from_raw_parts_mut(core::ptr::NonNull::<u8>::dangling().as_ptr(), 0)
}

/// Bytes the cartridge's save medium occupies (0 when it has none).
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_save_size(handle: *mut GbaMvii) -> usize {
    save_bytes(handle).len()
}

/// Copy the save medium out. Returns the number of bytes written.
///
/// # Safety
/// `handle` must be live; `out` must point at `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_save_read(
    handle: *mut GbaMvii,
    out: *mut u8,
    cap: usize,
) -> usize {
    if out.is_null() {
        return 0;
    }
    let data = save_bytes(handle);
    let n = data.len().min(cap);
    core::ptr::copy_nonoverlapping(data.as_ptr(), out, n);
    n
}

/// Restore a save. A short image fills from the start and leaves the rest as
/// the medium's erased state, which is how a game that grew its save file
/// between versions still boots. Returns the number of bytes taken.
///
/// # Safety
/// `handle` must be live; `data` must point at `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_save_load(
    handle: *mut GbaMvii,
    data: *const u8,
    len: usize,
) -> usize {
    if data.is_null() {
        return 0;
    }
    let dst = save_bytes(handle);
    let n = dst.len().min(len);
    core::ptr::copy_nonoverlapping(data, dst.as_mut_ptr(), n);
    n
}

/// FNV-1a over the save medium — the cheap "has anything changed?" the caller
/// needs to decide whether a flush to eMMC is worth stalling the machine for.
///
/// # Safety
/// `handle` must be live.
#[no_mangle]
pub unsafe extern "C" fn gba_mvii_save_hash(handle: *mut GbaMvii) -> u64 {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for &b in save_bytes(handle).iter() {
        h ^= b as u64;
        h = h.wrapping_mul(0x1_0000_01b3);
    }
    h
}
