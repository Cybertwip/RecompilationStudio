//! Low-overhead cost attribution for the PPU and APU work hidden inside
//! `gba_mvii_run_steps()`.
//!
//! A first implementation timed every visible scanline and every audio-grid
//! advance. On MVII the clock is a syscall plus an uncached GPT read, so the
//! profiler itself made 2,514 clock calls per GBA frame (160 lines + about
//! 1,097 samples, bracketed twice). The resulting "PPU/APU" numbers included
//! milliseconds of measurement overhead and the extra start-call cost leaked
//! into `rest`; the diagnostic could cut the frame rate it was trying to
//! explain.
//!
//! This version samples one line in 16 and one audio advance in 64. A one-shot
//! calibration measures the clock-call cost, which is subtracted from every
//! sampled span before its per-unit cost is scaled to the full line/sample
//! count. That reduces the instrumentation to roughly 54 clock calls per frame
//! while preserving the report-window totals the C++ runtime expects.

use core::sync::atomic::{AtomicU32, Ordering};

extern "C" {
    fn gba_mvii_host_now_us() -> u64;
}

const PPU_SAMPLE_MASK: u32 = 15; // 1 / 16 visible lines
const APU_SAMPLE_MASK: u64 = 63; // 1 / 64 audio-grid positions
const AUDIO_SAMPLE_CYCLES: u64 = crate::mem::AUDIO_SAMPLE_CYCLES;
const CLOCK_CALIBRATION_READS: u32 = 1024;

static CLOCK_NS: AtomicU32 = AtomicU32::new(0);
static CALIBRATION_SINK: AtomicU32 = AtomicU32::new(0);

static PPU_SAMPLE_NS: AtomicU32 = AtomicU32::new(0);
static PPU_SAMPLE_LINES: AtomicU32 = AtomicU32::new(0);
static PPU_TOTAL_LINES: AtomicU32 = AtomicU32::new(0);

static APU_SAMPLE_NS: AtomicU32 = AtomicU32::new(0);
static APU_SAMPLE_COUNT: AtomicU32 = AtomicU32::new(0);
static APU_TOTAL_COUNT: AtomicU32 = AtomicU32::new(0);

pub struct Span {
    start_us: u64,
    sampled: bool,
}

impl Span {
    #[inline]
    fn skipped() -> Span {
        Span {
            start_us: 0,
            sampled: false,
        }
    }

    #[inline]
    fn sampled() -> Span {
        Span {
            // SAFETY: a plain integer-returning C function with no arguments.
            start_us: unsafe { gba_mvii_host_now_us() },
            sampled: true,
        }
    }

    #[inline]
    fn elapsed_net_ns(self) -> u32 {
        if !self.sampled {
            return 0;
        }
        // The timestamp lies inside now_us(); the delta therefore contains
        // about one clock-call latency (the tail of the first call plus the
        // head of the second), not two. Subtract the calibrated one-call cost.
        let elapsed_us = unsafe { gba_mvii_host_now_us() }.wrapping_sub(self.start_us);
        let elapsed_ns = elapsed_us.saturating_mul(1000).min(u32::MAX as u64) as u32;
        elapsed_ns.saturating_sub(CLOCK_NS.load(Ordering::Relaxed))
    }

    #[inline]
    pub fn end_ppu(self, lines: u32) {
        PPU_TOTAL_LINES.fetch_add(lines, Ordering::Relaxed);
        if self.sampled {
            PPU_SAMPLE_NS.fetch_add(self.elapsed_net_ns(), Ordering::Relaxed);
            PPU_SAMPLE_LINES.fetch_add(lines, Ordering::Relaxed);
        }
    }

    #[inline]
    pub fn end_apu(self, samples: u32) {
        APU_TOTAL_COUNT.fetch_add(samples, Ordering::Relaxed);
        if self.sampled {
            APU_SAMPLE_NS.fetch_add(self.elapsed_net_ns(), Ordering::Relaxed);
            APU_SAMPLE_COUNT.fetch_add(samples, Ordering::Relaxed);
        }
    }
}

#[inline]
pub fn start_ppu(line: u32) -> Span {
    if line & PPU_SAMPLE_MASK == 0 {
        Span::sampled()
    } else {
        Span::skipped()
    }
}

#[inline]
pub fn start_apu(audio_cursor: u64) -> Span {
    if (audio_cursor / AUDIO_SAMPLE_CYCLES) & APU_SAMPLE_MASK == 0 {
        Span::sampled()
    } else {
        Span::skipped()
    }
}

/// Measure one host clock read and clear any counters from startup work.
pub fn calibrate() -> u32 {
    let start = unsafe { gba_mvii_host_now_us() };
    let mut sink = 0u32;
    for _ in 0..CLOCK_CALIBRATION_READS {
        sink ^= unsafe { gba_mvii_host_now_us() } as u32;
    }
    let elapsed_us = unsafe { gba_mvii_host_now_us() }.wrapping_sub(start);
    // From the first timestamp to the final one there are N inner calls plus
    // the final call interval. Keep nanoseconds so sub-microsecond clocks do
    // not round to zero before sampled spans subtract the cost.
    let intervals = CLOCK_CALIBRATION_READS as u64 + 1;
    let clock_ns = (elapsed_us.saturating_mul(1000) / intervals).min(u32::MAX as u64) as u32;
    CLOCK_NS.store(clock_ns, Ordering::Relaxed);
    CALIBRATION_SINK.store(sink, Ordering::Relaxed);
    reset();
    clock_ns
}

fn scaled_us(sample_ns: u32, sampled_units: u32, total_units: u32) -> u32 {
    if sampled_units == 0 || total_units == 0 {
        return 0;
    }
    ((sample_ns as u64 * total_units as u64) / sampled_units as u64 / 1000).min(u32::MAX as u64)
        as u32
}

fn reset() {
    PPU_SAMPLE_NS.store(0, Ordering::Relaxed);
    PPU_SAMPLE_LINES.store(0, Ordering::Relaxed);
    PPU_TOTAL_LINES.store(0, Ordering::Relaxed);
    APU_SAMPLE_NS.store(0, Ordering::Relaxed);
    APU_SAMPLE_COUNT.store(0, Ordering::Relaxed);
    APU_TOTAL_COUNT.store(0, Ordering::Relaxed);
}

/// Return estimated full-window PPU/APU microseconds plus exact line/sample
/// counts, then clear the window.
pub fn take() -> (u32, u32, u32, u32) {
    let ppu_sample_ns = PPU_SAMPLE_NS.swap(0, Ordering::Relaxed);
    let ppu_sample_lines = PPU_SAMPLE_LINES.swap(0, Ordering::Relaxed);
    let ppu_total_lines = PPU_TOTAL_LINES.swap(0, Ordering::Relaxed);
    let apu_sample_ns = APU_SAMPLE_NS.swap(0, Ordering::Relaxed);
    let apu_sample_count = APU_SAMPLE_COUNT.swap(0, Ordering::Relaxed);
    let apu_total_count = APU_TOTAL_COUNT.swap(0, Ordering::Relaxed);

    (
        scaled_us(ppu_sample_ns, ppu_sample_lines, ppu_total_lines),
        scaled_us(apu_sample_ns, apu_sample_count, apu_total_count),
        ppu_total_lines,
        apu_total_count,
    )
}
