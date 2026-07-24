fn main() {
    println!("cargo:rerun-if-env-changed=RECOMP_EXPORT_GIP_SYMBOLS");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos")
        || std::env::var_os("RECOMP_EXPORT_GIP_SYMBOLS").is_none()
    {
        return;
    }
    for symbol in [
        "_psx_gip_gamepad_open",
        "_psx_gip_gamepad_get_state",
        "_psx_gip_gamepad_close",
    ] {
        println!("cargo:rustc-link-arg=-Wl,-exported_symbol,{symbol}");
    }
}
