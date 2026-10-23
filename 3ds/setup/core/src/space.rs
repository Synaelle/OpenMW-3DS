//! The space an install takes, on the SD card and where it's prepared.

use std::path::Path;

use crate::bsa;
use crate::cooker;
use crate::prepare::{cook_options, folder_size, read_state, sound_fingerprint, ARCHIVES, CONTENT, STATE_FILE};

/// The cooked pack's size per texture, roughly (Morrowind alone: 81.9 MB for 5,239 textures).
const TEXTURE_BYTES: u64 = 16_000;
/// Room kept free beyond the estimate (the estimate's margin, the logs).
const MARGIN: u64 = 32 << 20;

#[derive(Clone, Debug, Default)]
pub struct SpacePlan {
    /// The install's whole size: what the SD card needs free (the textures' share estimated unless already cooked).
    pub install_size: u64,
    /// What this preparation writes in the output folder (less when it updates an install).
    pub to_write: u64,
    /// Free space where the output folder goes (None when it can't be told).
    pub available: Option<u64>,
}

impl SpacePlan {
    /// Whether the output's drive has room for what this preparation writes.
    pub fn fits(&self) -> bool {
        self.available.is_none_or(|free| free >= self.to_write + MARGIN)
    }
}

fn file_size(path: &Path) -> u64 {
    path.metadata().map(|m| m.len()).unwrap_or(0)
}

/// Free space on the drive a folder is (or will be) on: its closest existing parent's.
pub fn available_space(folder: &Path) -> Option<u64> {
    let mut existing = Some(folder);
    while let Some(path) = existing {
        if path.is_dir() {
            return fs4::available_space(path).ok();
        }
        existing = path.parent();
    }
    None
}

/// The size of the port's own files: the port folder without what a preparation adds to it.
fn port_files_size(port: &Path) -> u64 {
    let openmw = port.join("openmw");
    let mut made: Vec<_> = CONTENT.iter().chain(ARCHIVES).map(|f| openmw.join("data").join(f)).collect();
    made.push(openmw.join("data").join("sound3ds.bsa"));
    made.push(openmw.join("cooked").join("textures.o3tp"));
    made.push(openmw.join(STATE_FILE));
    let (size, _) = folder_size(port);
    let (logs, _) = folder_size(&openmw.join("logs"));
    size - made.iter().map(|f| file_size(f)).sum::<u64>().min(size) - logs.min(size)
}

/// How much an install takes, and what preparing it in `output` writes (the files it would keep don't count).
pub fn plan(morrowind: &Path, output: &Path, port: &Path) -> SpacePlan {
    let data_files = morrowind.join("Data Files");
    let openmw = output.join("openmw");
    let state = read_state(output);
    let mut install_size = 0;
    let mut to_write = 0;

    // The port's own files (nothing to copy when the output is the port folder, which then holds an install too).
    let port_size = port_files_size(port);
    install_size += port_size;
    if dunce::canonicalize(port).ok() != dunce::canonicalize(output).ok() {
        to_write += port_size;
    }

    // The game's files.
    for file in CONTENT.iter().chain(ARCHIVES) {
        let (from, to) = (data_files.join(file), openmw.join("data").join(file));
        let size = file_size(&from);
        install_size += size;
        if file_size(&to) != size {
            to_write += size;
        }
    }

    // The sounds.
    if let Ok(sounds) = bsa::sound_files(&data_files, &["Sound", "Music"]) {
        let size = bsa::archive_size(&sounds);
        install_size += size;
        let archive = openmw.join("data").join("sound3ds.bsa");
        if state.get("sound") != Some(&sound_fingerprint(&sounds)) || file_size(&archive) != size {
            to_write += size;
        }
    }

    // The textures: the pack already cooked from the same files, or an estimate.
    let pack = openmw.join("cooked").join("textures.o3tp");
    let options = cook_options(morrowind, port, pack.clone());
    let cooked = file_size(&pack);
    let fingerprint = cooker::source_fingerprint(&options).ok();
    let ready = cooked > 0
        && fingerprint.is_some_and(|f| state.get("textures") == Some(&format!("{f} {cooked}")));
    if ready {
        install_size += cooked;
    } else {
        let estimate = cooker::texture_count(&options).unwrap_or(5300) as u64 * TEXTURE_BYTES;
        install_size += estimate;
        to_write += estimate;
    }

    SpacePlan { install_size, to_write, available: available_space(output) }
}

/// A size for the player: "1.2 GB", "850 MB".
pub fn describe(bytes: u64) -> String {
    let mb = bytes as f64 / 1e6;
    if mb >= 1000.0 { format!("{:.1} GB", mb / 1000.0) } else { format!("{:.0} MB", mb.max(1.0)) }
}
