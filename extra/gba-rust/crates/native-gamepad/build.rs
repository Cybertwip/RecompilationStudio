use std::path::PathBuf;

fn main() {
    println!("cargo:rustc-check-cfg=cfg(recomp_native_gip)");
    println!("cargo:rerun-if-env-changed=RECOMP_GAMEPAD_ROOT");
    println!("cargo:rerun-if-env-changed=RECOMP_REQUIRE_GIP");

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return;
    }

    let crate_dir = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let default_root = crate_dir.join("../../../../lib/recomp_gamepad");
    let root = std::env::var_os("RECOMP_GAMEPAD_ROOT")
        .map(PathBuf::from)
        .unwrap_or(default_root);
    let source = root.join("src/gip_gamepad.cpp");
    let include = root.join("include");
    println!("cargo:rerun-if-changed={}", source.display());
    println!("cargo:rerun-if-changed={}", include.join("recomp_gamepad/gip_gamepad.h").display());

    let required = std::env::var_os("RECOMP_REQUIRE_GIP").is_some();
    if !source.is_file() || !include.is_dir() {
        if required {
            panic!("recomp_gamepad source is missing at {}", root.display());
        }
        println!("cargo:warning=recomp_gamepad source unavailable; macOS direct-USB input disabled");
        return;
    }

    let libusb = match pkg_config::Config::new()
        .statik(true)
        .cargo_metadata(true)
        .probe("libusb-1.0")
    {
        Ok(library) => library,
        Err(error) => {
            if required {
                panic!("libusb-1.0 is required for macOS direct-USB input: {error}");
            }
            println!("cargo:warning=libusb-1.0 unavailable; macOS direct-USB input disabled ({error})");
            return;
        }
    };

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++17")
        .define("PSX_HAVE_GIP_GAMEPAD", "1")
        .include(&include)
        .file(&source);
    for path in libusb.include_paths {
        build.include(path);
    }
    build.compile("recomp_gamepad_gip");
    println!("cargo:rustc-cfg=recomp_native_gip");
}
