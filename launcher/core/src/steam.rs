//! Steam's own files: where Garry's Mod is, which build, whether Steam is
//! busy with it, and the launch command.

use std::path::{Path, PathBuf};

pub const APP_ID: &str = "4000";

/// The quoted tokens of a Valve KeyValues text (`"key" "value"`, `{`, `}`),
/// with `\\` and `\"` unescaped. Comments and anything unquoted are skipped.
pub fn tokens(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                let mut s = String::new();
                while let Some(c) = chars.next() {
                    match c {
                        '"' => break,
                        '\\' => match chars.next() {
                            Some('\\') => s.push('\\'),
                            Some('"') => s.push('"'),
                            Some('n') => s.push('\n'),
                            Some('t') => s.push('\t'),
                            Some(o) => {
                                s.push('\\');
                                s.push(o);
                            }
                            None => {}
                        },
                        c => s.push(c),
                    }
                }
                out.push(s);
            }
            '{' | '}' => out.push(c.to_string()),
            '/' if chars.peek() == Some(&'/') => {
                for c in chars.by_ref() {
                    if c == '\n' {
                        break;
                    }
                }
            }
            _ => {}
        }
    }
    out
}

/// A KeyValues node: a string or a block of children.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Kv {
    Str(String),
    Obj(Vec<(String, Kv)>),
}

impl Kv {
    /// A child block or string by key (case-insensitive).
    pub fn get(&self, key: &str) -> Option<&Kv> {
        match self {
            Kv::Obj(items) => items.iter().find(|(k, _)| k.eq_ignore_ascii_case(key)).map(|(_, v)| v),
            Kv::Str(_) => None,
        }
    }
    pub fn str(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Some(Kv::Str(s)) => Some(s),
            _ => None,
        }
    }
    pub fn children(&self) -> &[(String, Kv)] {
        match self {
            Kv::Obj(items) => items,
            Kv::Str(_) => &[],
        }
    }
}

/// The whole text as one block (at most 16 levels deep; anything malformed
/// ends the block it's in).
pub fn parse_kv(text: &str) -> Kv {
    fn block(t: &[String], i: &mut usize, depth: usize) -> Vec<(String, Kv)> {
        let mut items = Vec::new();
        while *i < t.len() {
            if t[*i] == "}" {
                *i += 1;
                break;
            }
            let key = t[*i].clone();
            *i += 1;
            match t.get(*i).map(|s| s.as_str()) {
                Some("{") if depth < 16 => {
                    *i += 1;
                    items.push((key, Kv::Obj(block(t, i, depth + 1))));
                }
                Some("{") | None => break,
                Some(_) => {
                    items.push((key, Kv::Str(t[*i].clone())));
                    *i += 1;
                }
            }
        }
        items
    }
    let t = tokens(text);
    let mut i = 0;
    Kv::Obj(block(&t, &mut i, 0))
}

/// The first value of `key` (case-insensitive) at any depth.
pub fn value_of(tokens: &[String], key: &str) -> Option<String> {
    tokens
        .windows(2)
        .find(|w| w[0].eq_ignore_ascii_case(key) && w[1] != "{" && w[1] != "}")
        .map(|w| w[1].clone())
}

/// Every library folder in `libraryfolders.vdf`.
pub fn library_paths(vdf: &str) -> Vec<PathBuf> {
    let t = tokens(vdf);
    t.windows(2)
        .filter(|w| w[0].eq_ignore_ascii_case("path") && w[1] != "{")
        .map(|w| PathBuf::from(&w[1]))
        .collect()
}

/// What `appmanifest_4000.acf` says.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AppManifest {
    pub build_id: String,
    pub state_flags: u32,
    pub install_dir: String,
}

impl AppManifest {
    pub fn parse(acf: &str) -> AppManifest {
        let t = tokens(acf);
        AppManifest {
            build_id: value_of(&t, "buildid").unwrap_or_default(),
            state_flags: value_of(&t, "StateFlags").and_then(|v| v.parse().ok()).unwrap_or(0),
            install_dir: value_of(&t, "installdir").unwrap_or_default(),
        }
    }

    /// Fully installed, with no update, verify or download in progress
    /// (StateFlags is exactly StateFullyInstalled = 4).
    pub fn settled(&self) -> bool {
        self.state_flags == 4
    }
}

/// Where Garry's Mod is installed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Game {
    /// `steamapps\common\GarrysMod`: the game's root (`crashes\`, `gmod.exe`).
    pub root: PathBuf,
    /// `...\GarrysMod\garrysmod`.
    pub garrysmod: PathBuf,
    /// `steamapps\appmanifest_4000.acf`.
    pub manifest: PathBuf,
}

impl Game {
    pub fn read_manifest(&self) -> Option<AppManifest> {
        std::fs::read_to_string(&self.manifest).ok().map(|t| AppManifest::parse(&t))
    }

    /// The pack's DLLs in `garrysmod\lua\bin` that aren't there.
    pub fn missing_modules(&self) -> Vec<&'static str> {
        MODULES.iter().copied().filter(|m| !self.garrysmod.join("lua\\bin").join(m).is_file()).collect()
    }
}

/// The pack's files in `garrysmod\lua\bin` (the installer's `Modules`).
pub const MODULES: [&str; 4] =
    ["gmcl_meowy_win32.dll", "gmcl_meowy_win64.dll", "gmsv_meowy_boot_win32.dll", "gmsv_meowy_boot_win64.dll"];

/// Finds Garry's Mod in Steam's libraries (`steam_path` first, then every
/// library in `libraryfolders.vdf`): the one holding `appmanifest_4000.acf`.
pub fn find_game(steam_path: &Path) -> Option<Game> {
    let mut libraries = vec![steam_path.to_path_buf()];
    if let Ok(vdf) = std::fs::read_to_string(steam_path.join("steamapps").join("libraryfolders.vdf")) {
        libraries.extend(library_paths(&vdf));
    }
    for lib in libraries {
        let manifest = lib.join("steamapps").join(format!("appmanifest_{APP_ID}.acf"));
        let Ok(acf) = std::fs::read_to_string(&manifest) else { continue };
        let dir = AppManifest::parse(&acf).install_dir;
        let dir = if dir.is_empty() || dir.contains(['\\', '/', ':']) || dir == ".." { "GarrysMod".into() } else { dir };
        let root = lib.join("steamapps").join("common").join(dir);
        let garrysmod = root.join("garrysmod");
        if garrysmod.join("lua").is_dir() {
            return Some(Game { root, garrysmod, manifest });
        }
    }
    None
}

/// `steam.exe`'s arguments to start the game: `-applaunch 4000`, then ours.
/// Steam puts them before the player's own launch options.
pub fn launch_args(disable_lua_refresh: bool, connect: Option<&str>) -> Vec<String> {
    let mut args = vec!["-applaunch".to_string(), APP_ID.to_string()];
    if disable_lua_refresh {
        args.push("-disableluarefresh".into());
    }
    if let Some(address) = connect.and_then(crate::address::public) {
        args.push("+connect".into());
        args.push(address);
    }
    args
}

/// The URL that asks Steam to make the running game join `address`.
pub fn connect_url(address: &str) -> Option<String> {
    crate::address::public(address).map(|a| format!("steam://connect/{a}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    const LIBRARIES: &str = r#""libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps"
		{
			"228980"		"401011084"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
		"apps"
		{
			"4000"		"7282791160"
		}
	}
}"#;

    const ACF: &str = r#""AppState"
{
	"appid"		"4000"
	"name"		"Garry's Mod"
	"StateFlags"		"4"
	"installdir"		"GarrysMod"
	"buildid"		"26439081"
	"InstalledDepots"
	{
		"4001"
		{
			"manifest"		"8532700529041238050"
			"size"		"6779299417"
		}
	}
}"#;

    #[test]
    fn libraries() {
        assert_eq!(
            library_paths(LIBRARIES),
            vec![PathBuf::from("C:\\Program Files (x86)\\Steam"), PathBuf::from("D:\\SteamLibrary")]
        );
    }

    #[test]
    fn manifest() {
        let m = AppManifest::parse(ACF);
        assert_eq!(m, AppManifest { build_id: "26439081".into(), state_flags: 4, install_dir: "GarrysMod".into() });
        assert!(m.settled());
        let updating = AppManifest::parse(&ACF.replace("\"StateFlags\"\t\t\"4\"", "\"StateFlags\"\t\t\"1030\""));
        assert_eq!(updating.state_flags, 1030);
        assert!(!updating.settled());
        assert_eq!(AppManifest::parse("garbage"), AppManifest::default());
    }

    #[test]
    fn escapes_and_comments() {
        let t = tokens("// c \"x\"\n\"a\\\"b\" \"c\\\\d\" { }");
        assert_eq!(t, vec!["a\"b", "c\\d", "{", "}"]);
    }

    #[test]
    fn finds_the_game_in_a_second_library() {
        let base = std::env::temp_dir().join(format!("meowy_launcher_steam_{}", std::process::id()));
        let steam = base.join("Steam");
        let lib = base.join("Lib2");
        std::fs::create_dir_all(steam.join("steamapps")).unwrap();
        std::fs::create_dir_all(lib.join("steamapps\\common\\GarrysMod\\garrysmod\\lua")).unwrap();
        let vdf = format!("\"libraryfolders\" {{ \"1\" {{ \"path\" \"{}\" }} }}", lib.display().to_string().replace('\\', "\\\\"));
        std::fs::write(steam.join("steamapps\\libraryfolders.vdf"), vdf).unwrap();
        assert_eq!(find_game(&steam), None, "no manifest yet");
        std::fs::write(lib.join("steamapps\\appmanifest_4000.acf"), ACF).unwrap();
        let game = find_game(&steam).expect("found");
        assert_eq!(game.root, lib.join("steamapps\\common\\GarrysMod"));
        assert_eq!(game.garrysmod, game.root.join("garrysmod"));
        assert_eq!(game.missing_modules().len(), 4);
        std::fs::remove_dir_all(&base).unwrap();
    }

    #[test]
    fn launch() {
        assert_eq!(launch_args(true, None), vec!["-applaunch", "4000", "-disableluarefresh"]);
        assert_eq!(launch_args(false, Some("138.201.62.52:27015")), vec!["-applaunch", "4000", "+connect", "138.201.62.52:27015"]);
        // Anything that isn't a public address never reaches the command line.
        assert_eq!(launch_args(false, Some("1.2.3.4:27015 +exec evil")), vec!["-applaunch", "4000"]);
        assert_eq!(launch_args(false, Some("192.168.0.1:27015")), vec!["-applaunch", "4000"]);
        assert_eq!(connect_url("1.2.3.4:27015").as_deref(), Some("steam://connect/1.2.3.4:27015"));
        assert_eq!(connect_url("1.2.3.4:27015/pw"), None);
    }
}
