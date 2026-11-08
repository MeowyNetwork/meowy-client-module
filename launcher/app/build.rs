// The launcher's version is the pack's (src/version.h, which the DLLs' version
// resources use too): shown in the window and the log (MEOWY_PACK_VERSION) and
// put in MeowyLauncher.exe's own version resource (Tauri's config "version",
// through TAURI_CONFIG, which tauri-build and tauri's generate_context! both
// merge into tauri.conf.json). So nobody bumps it by hand.
fn pack_version() -> String {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/version.h");
    println!("cargo:rerun-if-changed={}", path.display());
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let version = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("#define MEOWY_VERSION ")?.trim().strip_prefix('"')?.strip_suffix('"').map(String::from))
        .unwrap_or_else(|| panic!("{}: no #define MEOWY_VERSION \"x.y.z\"", path.display()));
    let parts: Vec<&str> = version.split('.').collect();
    assert!(
        parts.len() == 3 && parts.iter().all(|p| !p.is_empty() && p.len() <= 5 && p.bytes().all(|b| b.is_ascii_digit())),
        "src/version.h: MEOWY_VERSION {version:?} isn't x.y.z"
    );
    version
}

fn main() {
    let version = pack_version();
    println!("cargo:rustc-env=MEOWY_PACK_VERSION={version}");
    let config = format!("{{\"version\":\"{version}\"}}");
    // For tauri-build (this process) and for generate_context! (the compiler's).
    std::env::set_var("TAURI_CONFIG", &config);
    println!("cargo:rustc-env=TAURI_CONFIG={config}");
    tauri_build::build()
}
