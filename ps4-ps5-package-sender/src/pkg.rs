//! PS4 / PS5 package metadata reader (header, param.sfo, icon0.png).

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

#[derive(Clone, Debug, Default)]
pub struct PkgMeta {
    pub title: String,
    pub title_id: String,
    pub content_id: String,
    pub category: String,
    pub app_ver: String,
    pub version: String,
    pub sys_ver: String,
    pub platform: String,
    pub icon: Option<(u64, u32)>,
    pub valid: bool,
}

const PS4_MAGIC: u32 = 0x7F43_4E54;
const PS5_MAGIC: u32 = 0x7F46_4948;
const ENTRY_SFO: u32 = 0x1000;
const ENTRY_ICON: u32 = 0x1200;
const ENTRY_ICON_LOC_START: u32 = 0x1220;
const ENTRY_ICON_LOC_END: u32 = 0x123F;

fn is_image_magic(b: &[u8]) -> bool {
    (b.len() >= 8 && &b[..4] == b"\x89PNG")
        || (b.len() >= 4 && &b[..3] == b"\xFF\xD8\xFF")
        || (b.len() >= 12 && &b[..4] == b"RIFF" && &b[8..12] == b"WEBP")
}

fn be32(b: &[u8], o: usize) -> u32 {
    u32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn le16(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}

fn is_content_id(s: &str) -> bool {
    // XX0000-XXXX00000_00-XXXXXXXXXXXXXXXX
    let b = s.as_bytes();
    b.len() == 36
        && b[..2].iter().all(|c| c.is_ascii_uppercase())
        && b[2..6].iter().all(|c| c.is_ascii_digit())
        && b[6] == b'-'
        && b[7..11].iter().all(|c| c.is_ascii_uppercase())
        && b[11..16].iter().all(|c| c.is_ascii_digit())
        && b[16] == b'_'
        && b[19] == b'-'
}

pub fn title_id_from_name(name: &str) -> String {
    let b = name.as_bytes();
    for i in 0..b.len().saturating_sub(8) {
        let w = &b[i..i + 9];
        if w[..4].iter().all(|c| c.is_ascii_uppercase())
            && w[4..].iter().all(|c| c.is_ascii_digit())
        {
            let pre = &name[i..i + 4];
            if matches!(
                pre,
                "CUSA" | "PPSA" | "PCAS" | "PLAS" | "CUSB" | "PCJS" | "PLJS" | "PCKS" | "NPXS"
            ) {
                return name[i..i + 9].to_string();
            }
        }
    }
    String::new()
}

fn read_at(f: &mut File, off: u64, len: usize) -> Option<Vec<u8>> {
    f.seek(SeekFrom::Start(off)).ok()?;
    let mut v = vec![0u8; len];
    f.read_exact(&mut v).ok()?;
    Some(v)
}

pub fn parse_sfo(b: &[u8], meta: &mut PkgMeta) -> bool {
    if b.len() < 20 || &b[..4] != b"\0PSF" {
        return false;
    }
    let key_tab = le32(b, 8) as usize;
    let data_tab = le32(b, 12) as usize;
    let count = le32(b, 16) as usize;
    for i in 0..count.min(256) {
        let e = 20 + i * 16;
        if e + 16 > b.len() {
            break;
        }
        let key_off = key_tab + le16(b, e) as usize;
        let fmt = le16(b, e + 2);
        let len = le32(b, e + 4) as usize;
        let data_off = data_tab + le32(b, e + 12) as usize;
        if key_off >= b.len() || data_off + len > b.len() {
            continue;
        }
        let key_end = b[key_off..]
            .iter()
            .position(|&c| c == 0)
            .map(|p| key_off + p)
            .unwrap_or(b.len());
        let key = String::from_utf8_lossy(&b[key_off..key_end]).to_string();
        let raw = &b[data_off..data_off + len];
        let s = || {
            String::from_utf8_lossy(raw)
                .trim_end_matches('\0')
                .trim()
                .to_string()
        };
        match (key.as_str(), fmt) {
            ("TITLE", _) => meta.title = s(),
            ("TITLE_ID", _) => meta.title_id = s(),
            ("CONTENT_ID", _) => meta.content_id = s(),
            ("CATEGORY", _) => meta.category = s(),
            ("APP_VER", _) => meta.app_ver = s(),
            ("VERSION", _) => meta.version = s(),
            ("SYSTEM_VER", 0x0404) if len >= 4 => {
                let v = le32(raw, 0);
                if v != 0 {
                    meta.sys_ver = format!("{:x}.{:02x}", v >> 24, (v >> 16) & 0xFF);
                }
            }
            _ => {}
        }
    }
    true
}

pub fn read_meta(path: &Path) -> PkgMeta {
    let mut meta = PkgMeta::default();
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_default();
    if let Some(m) = try_read(path) {
        meta = m;
    }
    if meta.title_id.is_empty() && meta.content_id.len() >= 16 {
        meta.title_id = meta.content_id[7..16].to_string();
    }
    if meta.title_id.is_empty() {
        meta.title_id = title_id_from_name(&name);
    }
    if meta.platform.is_empty() {
        meta.platform = if meta.title_id.starts_with("PPSA") {
            "PS5".into()
        } else {
            "PS4".into()
        };
    }
    meta
}

fn try_read(path: &Path) -> Option<PkgMeta> {
    let mut f = File::open(path).ok()?;
    let mut head = [0u8; 0x1000];
    let n = f.read(&mut head).ok()?;
    if n < 0x80 {
        return None;
    }
    let mut meta = PkgMeta::default();
    let magic = be32(&head, 0);
    let cid = String::from_utf8_lossy(&head[0x40..0x64])
        .trim_end_matches('\0')
        .to_string();
    if is_content_id(&cid) {
        meta.content_id = cid;
    }
    match magic {
        PS4_MAGIC => meta.platform = "PS4".into(),
        PS5_MAGIC => {
            meta.platform = "PS5".into();
            meta.valid = true;
            return Some(meta);
        }
        _ => return Some(meta),
    }
    let entry_count = be32(&head, 0x10) as usize;
    let table_off = be32(&head, 0x18) as u64;
    if entry_count == 0 || entry_count > 10_000 {
        return Some(meta);
    }
    let table = read_at(&mut f, table_off, entry_count * 32)?;
    let mut sfo: Option<(u64, u32)> = None;
    let mut icon_main: Option<(u64, u32)> = None;
    let mut icon_loc: Vec<(u64, u32)> = Vec::new();
    for i in 0..entry_count {
        let e = i * 32;
        let id = be32(&table, e);
        let off = be32(&table, e + 16) as u64;
        let size = be32(&table, e + 20);
        match id {
            ENTRY_SFO => sfo = Some((off, size)),
            ENTRY_ICON if size > 12 && size < 4 * 1024 * 1024 => icon_main = Some((off, size)),
            ENTRY_ICON_LOC_START..=ENTRY_ICON_LOC_END if size > 12 && size < 4 * 1024 * 1024 => {
                icon_loc.push((off, size));
            }
            _ => {}
        }
    }
    for cand in icon_main.into_iter().chain(icon_loc) {
        if let Some(hdr) = read_at(&mut f, cand.0, 12) {
            if is_image_magic(&hdr) {
                meta.icon = Some(cand);
                break;
            }
        }
    }
    if let Some((off, size)) = sfo {
        if size > 0 && size < 1024 * 1024 {
            if let Some(b) = read_at(&mut f, off, size as usize) {
                meta.valid = parse_sfo(&b, &mut meta);
            }
        }
    }
    Some(meta)
}

pub fn read_icon(path: &Path, icon: (u64, u32)) -> Option<Vec<u8>> {
    let mut f = File::open(path).ok()?;
    let b = read_at(&mut f, icon.0, icon.1 as usize)?;
    is_image_magic(&b).then_some(b)
}

pub fn category_label(cat: &str) -> &'static str {
    match cat {
        "gd" | "gde" => "Game",
        "gp" => "Patch",
        "ac" => "DLC",
        "gda" | "gdc" => "App",
        "gdd" | "gdk" => "System",
        "" => "PKG",
        _ => "PKG",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ids() {
        assert!(is_content_id("UP0000-CUSA00000_00-XXXXXXXXXXXXXXXX"));
        assert_eq!(title_id_from_name("Gra_CUSA12345_v1.00.pkg"), "CUSA12345");
    }
}
