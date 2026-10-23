//! The language of a Morrowind.esm.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
use std::path::Path;

fn get32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

/// The game setting's string, without its trailing NULs.
fn trimmed(data: &[u8]) -> Vec<u8> {
    let end = data.iter().rposition(|&c| c != 0).map_or(0, |i| i + 1);
    data[..end].to_vec()
}

/// The language of a Morrowind.esm, read from its own interface words (the game settings sYes, sNo and sCancel)
/// rather than a list of file hashes (the English game exists in several versions). "English", the name of another
/// language it recognises ("German", "French"...), or None when it can't be told (an unreadable file, a language it
/// doesn't know).
pub fn esm_language(esm: &Path) -> Option<&'static str> {
    let mut file = BufReader::new(File::open(esm).ok()?);
    // TES3 records: 16-byte header (type, size, 8 bytes), then subrecords (type, size, data). The game settings come
    // early in Morrowind.esm (within its first ~1,500 records): stop when the three are found, or after enough
    // records.
    let mut words: BTreeMap<Vec<u8>, Vec<u8>> = BTreeMap::new();
    let mut data = Vec::new();
    for _ in 0..20000 {
        if words.len() >= 3 {
            break;
        }
        let mut header = [0u8; 16];
        if file.read_exact(&mut header).is_err() {
            break;
        }
        let size = get32(&header[4..]);
        if &header[..4] != b"GMST" {
            if file.seek(SeekFrom::Current(i64::from(size))).is_err() {
                break;
            }
            continue;
        }
        data.resize(size as usize, 0);
        if file.read_exact(&mut data).is_err() {
            break;
        }
        let (mut id, mut value) = (Vec::new(), Vec::new());
        let mut p = 0;
        while p + 8 <= data.len() {
            let length = get32(&data[p + 4..]) as usize;
            if p + 8 + length > data.len() {
                break;
            }
            match &data[p..p + 4] {
                b"NAME" => id = trimmed(&data[p + 8..p + 8 + length]),
                b"STRV" => value = trimmed(&data[p + 8..p + 8 + length]),
                _ => {}
            }
            p += 8 + length;
        }
        if matches!(id.as_slice(), b"sYes" | b"sNo" | b"sCancel") {
            words.insert(id, value);
        }
    }
    let word = |id: &[u8]| words.get(id).map(Vec::as_slice).unwrap_or_default();
    if word(b"sYes") == b"Yes" && word(b"sNo") == b"No" && word(b"sCancel") == b"Cancel" {
        return Some("English");
    }
    // The game's own translations (their single-byte encodings: Windows-1252, -1250 or -1251).
    match word(b"sYes") {
        b"Ja" => Some("German"),
        b"Oui" => Some("French"),
        b"S\xEC" => Some("Italian"),
        b"S\xED" => Some("Spanish"),
        b"Tak" => Some("Polish"),
        b"\xC4\xE0" => Some("Russian"),
        _ => None,
    }
}

/// The interface language code for a Morrowind edition's language: "en", "fr" or "de". None for an edition this
/// version doesn't support yet.
pub fn locale_for(language: &str) -> Option<&'static str> {
    match language {
        "English" => Some("en"),
        "French" => Some("fr"),
        "German" => Some("de"),
        _ => None,
    }
}
