//! OpenMW's fallback values from a Morrowind.ini.

use std::collections::BTreeMap;
use std::path::Path;

use crate::text::{cp1252_to_utf8, trim};
use crate::{io_error, Result};

/// OpenMW's list of Morrowind.ini keys it imports ("Section:Key", from apps/mwiniimporter/importer.cpp, refreshed
/// by tools/update_fallback_keys.py).
const KEYS: &str = include_str!("../../data/fallback_keys.txt");

fn keys() -> impl Iterator<Item = &'static str> {
    KEYS.lines().map(str::trim_end).filter(|line| !line.is_empty())
}

/// How many keys OpenMW imports (for the summary).
pub fn fallback_key_count() -> usize {
    keys().count()
}

/// OpenMW's fallback values from a Morrowind.ini, as its mwiniimporter writes them: for each key it imports
/// ("Section:Key"), every value the file gives, as "fallback=Section_Key,value" with spaces turned into '_'. The
/// file is read as Windows-1252 and the lines come out in UTF-8.
pub fn read_fallbacks(morrowind_ini: &Path) -> Result<Vec<String>> {
    let raw = std::fs::read(morrowind_ini).map_err(|e| io_error("can't read", morrowind_ini, e))?;

    // "Section:Key" -> every value, in file order (MwIniImporter::loadIniFile).
    let mut values: BTreeMap<Vec<u8>, Vec<String>> = BTreeMap::new();
    let mut section: Vec<u8> = Vec::new();
    for line in raw.split(|&c| c == b'\n') {
        let line = trim(line);
        if line.is_empty() || line[0] == b';' {
            continue;
        }
        if line[0] == b'[' && line[line.len() - 1] == b']' {
            section = line[1..line.len() - 1].to_vec();
            continue;
        }
        let Some(equals) = line.iter().position(|&c| c == b'=') else {
            continue;
        };
        let key = trim(&line[..equals]);
        if key.is_empty() {
            continue;
        }
        let mut name = section.clone();
        name.push(b':');
        name.extend_from_slice(key);
        values.entry(name).or_default().push(cp1252_to_utf8(trim(&line[equals + 1..])));
    }

    let mut lines = Vec::new();
    for key in keys() {
        let Some(found) = values.get(key.as_bytes()) else {
            continue;
        };
        let name: String = key.chars().map(|c| if c == ' ' || c == ':' { '_' } else { c }).collect();
        for value in found {
            lines.push(format!("fallback={name},{value}"));
        }
    }
    Ok(lines)
}
