//! Whether a DLL exports a name, read from the file (never loaded): used to
//! tell a start-up stub that can load updates (`meowy_stub_version`, src/boot)
//! from an older one.

/// True if the PE file's export table names `name`. Any malformed or
/// unexpected layout is false.
pub fn exports(file: &[u8], name: &str) -> bool {
    (|| -> Option<bool> {
        let u16_at = |o: usize| file.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]));
        let u32_at = |o: usize| file.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
        if file.get(..2)? != b"MZ" {
            return Some(false);
        }
        let pe = u32_at(0x3C)? as usize;
        if file.get(pe..pe + 4)? != b"PE\0\0" {
            return Some(false);
        }
        let coff = pe + 4;
        let sections = u16_at(coff + 2)? as usize;
        let opt_size = u16_at(coff + 16)? as usize;
        let opt = coff + 20;
        let dirs = match u16_at(opt)? {
            0x10B => opt + 96,  // PE32
            0x20B => opt + 112, // PE32+
            _ => return Some(false),
        };
        let export_rva = u32_at(dirs)? as usize;
        if export_rva == 0 {
            return Some(false);
        }
        let table = opt + opt_size;
        let to_offset = |rva: usize| -> Option<usize> {
            (0..sections.min(96)).find_map(|i| {
                let s = table + i * 40;
                let (vsize, va, raw_size, raw) =
                    (u32_at(s + 8)? as usize, u32_at(s + 12)? as usize, u32_at(s + 16)? as usize, u32_at(s + 20)? as usize);
                (rva >= va && rva < va + vsize.max(raw_size)).then(|| raw + (rva - va))
            })
        };
        let dir = to_offset(export_rva)?;
        let count = (u32_at(dir + 24)? as usize).min(65536);
        let names = to_offset(u32_at(dir + 32)? as usize)?;
        for i in 0..count {
            let at = to_offset(u32_at(names + i * 4)? as usize)?;
            let end = at + file.get(at..)?.iter().take(256).position(|&b| b == 0)?;
            if &file[at..end] == name.as_bytes() {
                return Some(true);
            }
        }
        Some(false)
    })()
    .unwrap_or(false)
}

#[cfg(all(test, windows))]
mod tests {
    #[test]
    fn reads_a_real_export_table() {
        let sys = std::env::var("SystemRoot").unwrap_or_else(|_| "C:\\Windows".into());
        let k32 = std::fs::read(format!("{sys}\\System32\\kernel32.dll")).unwrap();
        assert!(super::exports(&k32, "LoadLibraryW"));
        assert!(super::exports(&k32, "GetProcAddress"));
        assert!(!super::exports(&k32, "meowy_stub_version"));
        assert!(!super::exports(&k32, "LoadLibrary"), "the whole name, not a prefix");
        let notepad = std::fs::read(format!("{sys}\\System32\\notepad.exe")).unwrap_or_default();
        assert!(!super::exports(&notepad, "LoadLibraryW"));
        assert!(!super::exports(b"MZ garbage", "x") && !super::exports(b"", "x"));
    }
}
