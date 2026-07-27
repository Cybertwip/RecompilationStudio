//! Host wall-clock access for the cartridge RTC.
//!
//! The GBA real-time-clock chip (an S-3511A on the cartridge) is a battery-
//! backed CMOS clock. We don't emulate a battery — we forward the request to
//! the host's own clock, broken down into *local* civil time, which is what
//! these games display and key their day/night and calendar events off.
//!
//! Per-platform, by design (no third-party time/zone crate):
//!   - Unix (macOS, Linux, Android, the BSDs): `localtime_r(3)` via libc.
//!     POSIX, present on every libc (glibc, musl, bionic, BSD libc) — no
//!     distro assumptions. It honours `/etc/localtime` and `$TZ`.
//!   - Windows: `GetLocalTime` (Win32), already timezone-resolved.
//!   - Anything else (e.g. wasm): a fixed epoch so the crate still builds.
//!
//! The civil <-> linear-seconds helpers (Howard Hinnant's public-domain
//! day-count algorithms) are used only for delta arithmetic when a game
//! *sets* the clock — see `rtc::Rtc`. They are pure proleptic-Gregorian
//! math and round-trip exactly; no timezone logic rides on them, because
//! `now_local()` has already applied the zone.

/// Broken-down local civil time. `dow` is 0=Sunday..6=Saturday (matching
/// both `tm_wday` and Win32 `wDayOfWeek`).
// MVII: `no_std` build — see nostd.rs. This one line replaces the std
// prelude the crate loses; nothing below it differs from upstream.
use crate::nostd::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Civil {
    pub year: i32,
    pub month: u8,
    pub day: u8,
    pub dow: u8,
    pub hour: u8,
    pub min: u8,
    pub sec: u8,
}

/// Fallback used if the platform call fails or the target has no clock API.
/// 2000-01-01 was a Saturday (dow = 6).
const FALLBACK: Civil = Civil {
    year: 2000,
    month: 1,
    day: 1,
    dow: 6,
    hour: 0,
    min: 0,
    sec: 0,
};

extern "C" {
    /// Local civil time as seconds on the same proleptic-Gregorian timeline
    /// `to_linear`/`from_linear` use — i.e. already zone-adjusted, because
    /// there is nothing on this side that could adjust it. Supplied by the
    /// runtime (rust/gba-mvii/src/lib.rs), which reads MVII's clock.
    /// A negative return means "no clock", and we fall back.
    fn gba_mvii_host_epoch() -> i64;
}

/// MVII replaces the upstream `localtime_r`/`GetLocalTime` split.
///
/// The handheld has one clock and no timezone database: `time()` on MVII
/// already returns local wall time (see the `time` stub in
/// Dash/llvm_libc_stubs.cpp), so the zone conversion the desktop build does
/// has nothing left to do here. Breaking it down is Hinnant's day-count
/// arithmetic below, which is the same code the set-clock path uses — so a
/// game that reads the clock and a game that sets it agree by construction.
pub fn now_local() -> Civil {
    // SAFETY: a plain integer-returning C function with no arguments.
    let secs = unsafe { gba_mvii_host_epoch() };
    if secs < 0 {
        return FALLBACK;
    }
    from_linear(secs)
}

/// Civil time -> seconds on a single proleptic-Gregorian timeline. Used only
/// for relative arithmetic (set-clock offset), so the absolute epoch is
/// irrelevant as long as it round-trips with `from_linear`.
pub fn to_linear(c: &Civil) -> i64 {
    days_from_civil(c.year as i64, c.month as i64, c.day as i64) * 86_400
        + c.hour as i64 * 3600
        + c.min as i64 * 60
        + c.sec as i64
}

/// Inverse of `to_linear`. Recomputes the day-of-week from the day count
/// (1970-01-01 was a Thursday).
pub fn from_linear(s: i64) -> Civil {
    let days = s.div_euclid(86_400);
    let rem = s.rem_euclid(86_400);
    let (y, m, d) = civil_from_days(days);
    Civil {
        year: y as i32,
        month: m as u8,
        day: d as u8,
        dow: (days + 4).rem_euclid(7) as u8,
        hour: (rem / 3600) as u8,
        min: (rem % 3600 / 60) as u8,
        sec: (rem % 60) as u8,
    }
}

/// Days since 1970-01-01 (Hinnant). Valid for the full i64 range.
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400; // [0, 399]
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1; // [0, 365]
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    era * 146_097 + doe - 719_468
}

/// Inverse of `days_from_civil`: day count -> (year, month, day).
fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365; // [0, 399]
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = doy - (153 * mp + 2) / 5 + 1; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 }; // [1, 12]
    (if m <= 2 { y + 1 } else { y }, m, d)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn linear_roundtrips() {
        let c = Civil {
            year: 2026,
            month: 6,
            day: 11,
            dow: 4,
            hour: 13,
            min: 37,
            sec: 9,
        };
        assert_eq!(from_linear(to_linear(&c)), c);
    }

    #[test]
    fn weekday_is_correct() {
        // 2000-01-01 was a Saturday (6); 2026-06-11 is a Thursday (4).
        assert_eq!(from_linear(to_linear(&FALLBACK)).dow, 6);
        let thu = Civil {
            year: 2026,
            month: 6,
            day: 11,
            dow: 0,
            hour: 0,
            min: 0,
            sec: 0,
        };
        assert_eq!(from_linear(to_linear(&thu)).dow, 4);
    }

    #[test]
    fn add_a_day_crosses_month_and_weekday() {
        // 2024-02-28 + 2 days = 2024-03-01 (leap year).
        let feb = Civil {
            year: 2024,
            month: 2,
            day: 28,
            dow: 0,
            hour: 23,
            min: 0,
            sec: 0,
        };
        let later = from_linear(to_linear(&feb) + 2 * 86_400);
        assert_eq!((later.year, later.month, later.day), (2024, 3, 1));
    }

    #[test]
    fn now_local_is_sane() {
        // Whatever the host says, the fields must be in range.
        let c = now_local();
        assert!((1..=12).contains(&c.month));
        assert!((1..=31).contains(&c.day));
        assert!(c.hour < 24 && c.min < 60 && c.sec < 60 && c.dow < 7);
    }
}
