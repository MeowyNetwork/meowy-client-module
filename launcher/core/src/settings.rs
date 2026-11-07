//! The launcher's settings (`launcher\settings.json`) and its folders.
//! "Start at sign-in" isn't stored here: the Run value is the truth.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    /// Add `-disableluarefresh` to launches (start-up #1: 0.8-1.6 s per start).
    pub disable_lua_refresh: bool,
    /// Show a card when Meowy Network answers again after being down (off by default).
    pub notify_back_up: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { disable_lua_refresh: true, notify_back_up: false }
    }
}

impl Settings {
    /// Missing or unreadable: the defaults.
    pub fn load(dir: &Path) -> Settings {
        std::fs::read(dir.join("settings.json"))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_default()
    }

    pub fn save(&self, dir: &Path) -> std::io::Result<()> {
        std::fs::create_dir_all(dir)?;
        let temp = dir.join("settings.json.tmp");
        std::fs::write(&temp, serde_json::to_vec_pretty(self).expect("plain struct"))?;
        std::fs::rename(&temp, dir.join("settings.json"))
    }
}

/// `%LOCALAPPDATA%\MeowyClientPack`: the pack's own files (never
/// `garrysmod\data`, which every server's Lua can read).
pub fn pack_data_dir() -> Option<PathBuf> {
    std::env::var_os("LOCALAPPDATA").filter(|v| !v.is_empty()).map(|v| PathBuf::from(v).join("MeowyClientPack"))
}

/// `%LOCALAPPDATA%\MeowyClientPack\launcher`: where the installer puts the
/// launcher, and its settings, log and WebView2 profile.
pub fn launcher_dir() -> Option<PathBuf> {
    pack_data_dir().map(|d| d.join("launcher"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_and_defaults() {
        let dir = std::env::temp_dir().join(format!("meowy_launcher_settings_{}", std::process::id()));
        assert_eq!(Settings::load(&dir), Settings::default());
        assert!(Settings::default().disable_lua_refresh, "on by default, with an opt-out");
        assert!(!Settings::default().notify_back_up, "off by default");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("settings.json"), "{\"disable_lua_refresh\": false}").unwrap();
        assert_eq!(Settings::load(&dir), Settings { disable_lua_refresh: false, notify_back_up: false }, "older file");
        let off = Settings { disable_lua_refresh: false, notify_back_up: true };
        off.save(&dir).unwrap();
        assert_eq!(Settings::load(&dir), off);
        std::fs::write(dir.join("settings.json"), "{ not json").unwrap();
        assert_eq!(Settings::load(&dir), Settings::default());
        std::fs::write(dir.join("settings.json"), "{\"future_setting\": 1}").unwrap();
        assert_eq!(Settings::load(&dir), Settings::default(), "unknown keys ignored");
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
