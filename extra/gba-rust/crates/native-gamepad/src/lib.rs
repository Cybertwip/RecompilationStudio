//! Optional native controller transports shared with the C++ recomp runtimes.
//!
//! On macOS this crate compiles and statically links the repository's
//! `recomp_gamepad` Xbox GIP backend. Other targets expose the same safe API
//! with an empty device list, so callers can layer it under their ordinary
//! SDL/gilrs path without platform conditionals.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr::NonNull;

pub const SELECTOR_CAPACITY: usize = 128;
pub const NAME_CAPACITY: usize = 128;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeviceInfo {
    pub selector: String,
    pub name: String,
    pub vendor_id: u16,
    pub product_id: u16,
}

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct State {
    pub buttons: u16,
    pub guide: bool,
    pub left_trigger: u16,
    pub right_trigger: u16,
    pub left_x: i16,
    pub left_y: i16,
    pub right_x: i16,
    pub right_y: i16,
}

pub mod button {
    pub const MENU: u16 = 0x0004;
    pub const VIEW: u16 = 0x0008;
    pub const A: u16 = 0x0010;
    pub const B: u16 = 0x0020;
    pub const X: u16 = 0x0040;
    pub const Y: u16 = 0x0080;
    pub const DPAD_UP: u16 = 0x0100;
    pub const DPAD_DOWN: u16 = 0x0200;
    pub const DPAD_LEFT: u16 = 0x0400;
    pub const DPAD_RIGHT: u16 = 0x0800;
    pub const LEFT_BUMPER: u16 = 0x1000;
    pub const RIGHT_BUMPER: u16 = 0x2000;
    pub const LEFT_STICK: u16 = 0x4000;
    pub const RIGHT_STICK: u16 = 0x8000;
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawInfo {
    selector: [c_char; SELECTOR_CAPACITY],
    name: [c_char; NAME_CAPACITY],
    vendor_id: u16,
    product_id: u16,
    bus: u8,
    address: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawState {
    buttons: u16,
    guide: u8,
    left_trigger: u16,
    right_trigger: u16,
    left_x: i16,
    left_y: i16,
    right_x: i16,
    right_y: i16,
}

#[cfg(recomp_native_gip)]
unsafe extern "C" {
    fn psx_gip_gamepad_enumerate(out: *mut RawInfo, capacity: usize) -> usize;
    fn psx_gip_gamepad_selector_supported(selector: *const c_char) -> c_int;
    fn psx_gip_gamepad_open(selector: *const c_char) -> *mut c_void;
    fn psx_gip_gamepad_close(gamepad: *mut c_void);
    fn psx_gip_gamepad_get_state(gamepad: *const c_void, out: *mut RawState) -> c_int;
}

fn raw_text(bytes: &[c_char]) -> String {
    let ptr = bytes.as_ptr();
    // The native ABI always NUL-terminates both fixed-capacity arrays.
    unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned()
}

pub fn available() -> bool {
    cfg!(recomp_native_gip)
}

pub fn enumerate() -> Vec<DeviceInfo> {
    #[cfg(recomp_native_gip)]
    {
        let count = unsafe { psx_gip_gamepad_enumerate(std::ptr::null_mut(), 0) };
        let mut raw = vec![RawInfo {
            selector: [0; SELECTOR_CAPACITY],
            name: [0; NAME_CAPACITY],
            vendor_id: 0,
            product_id: 0,
            bus: 0,
            address: 0,
        }; count.min(64)];
        let total = unsafe { psx_gip_gamepad_enumerate(raw.as_mut_ptr(), raw.len()) };
        raw.truncate(total.min(raw.len()));
        return raw
            .into_iter()
            .map(|info| DeviceInfo {
                selector: raw_text(&info.selector),
                name: raw_text(&info.name),
                vendor_id: info.vendor_id,
                product_id: info.product_id,
            })
            .collect();
    }
    #[cfg(not(recomp_native_gip))]
    Vec::new()
}

pub struct Gamepad {
    handle: NonNull<c_void>,
    selector: String,
}

impl Gamepad {
    pub fn open(selector: &str) -> Option<Self> {
        #[cfg(recomp_native_gip)]
        {
            let selector_c = CString::new(selector).ok()?;
            if unsafe { psx_gip_gamepad_selector_supported(selector_c.as_ptr()) } == 0 {
                return None;
            }
            let handle = NonNull::new(unsafe { psx_gip_gamepad_open(selector_c.as_ptr()) })?;
            return Some(Self {
                handle,
                selector: selector.to_string(),
            });
        }
        #[cfg(not(recomp_native_gip))]
        {
            let _ = selector;
            None
        }
    }

    pub fn selector(&self) -> &str {
        &self.selector
    }

    pub fn state(&self) -> Option<State> {
        #[cfg(recomp_native_gip)]
        {
            let mut raw = RawState::default();
            if unsafe { psx_gip_gamepad_get_state(self.handle.as_ptr(), &mut raw) } == 0 {
                return None;
            }
            return Some(State {
                buttons: raw.buttons,
                guide: raw.guide != 0,
                left_trigger: raw.left_trigger,
                right_trigger: raw.right_trigger,
                left_x: raw.left_x,
                left_y: raw.left_y,
                right_x: raw.right_x,
                right_y: raw.right_y,
            });
        }
        #[cfg(not(recomp_native_gip))]
        None
    }
}

impl Drop for Gamepad {
    fn drop(&mut self) {
        #[cfg(recomp_native_gip)]
        unsafe {
            psx_gip_gamepad_close(self.handle.as_ptr());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unavailable_targets_are_neutral() {
        if !available() {
            assert!(enumerate().is_empty());
            assert!(Gamepad::open("gip:auto").is_none());
        }
    }
}
