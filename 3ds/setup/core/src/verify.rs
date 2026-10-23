//! Checking the copy on the SD card: every file of the prepared folder there, whole.

use std::path::{Path, PathBuf};

use crate::bsa::walk_files;
use crate::{io_error, Result};

#[derive(Clone, Debug, Default)]
pub struct CopyCheck {
    /// The SD card's root that was compared (the folder picked, or its parent when the "openmw" folder was).
    pub root: PathBuf,
    /// The files compared.
    pub files: usize,
    /// Missing from the SD card, then there with another size (paths relative to the root, with '/').
    pub missing: Vec<String>,
    pub different: Vec<String>,
}

impl CopyCheck {
    pub fn complete(&self) -> bool {
        self.missing.is_empty() && self.different.is_empty()
    }
}

/// Files the game rewrites on the SD card (the Options' settings) or the setup's own: their sizes may differ.
fn skipped(relative: &str) -> bool {
    relative == "openmw/config/settings.cfg" || relative.starts_with("openmw/logs/") || relative.ends_with(".part")
}

/// The SD card's root for a folder the player picked: the "openmw" folder's parent when they picked that one.
pub fn sd_root(picked: &Path) -> PathBuf {
    if !picked.join("openmw").is_dir() && picked.file_name().is_some_and(|name| name.eq_ignore_ascii_case("openmw")) {
        if let Some(parent) = picked.parent() {
            return parent.to_path_buf();
        }
    }
    picked.to_path_buf()
}

/// Compares the prepared folder with its copy on the SD card, by size (contents would take as long as copying).
pub fn check_copy(output: &Path, picked: &Path) -> Result<CopyCheck> {
    let root = sd_root(picked);
    let mut files = Vec::new();
    walk_files(output, &mut files).map_err(|e| io_error("can't read", output, e))?;
    let mut check = CopyCheck { root: root.clone(), ..CopyCheck::default() };
    for file in files {
        let Ok(relative) = file.strip_prefix(output) else {
            continue;
        };
        let name: Vec<String> = relative.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect();
        let name = name.join("/");
        if skipped(&name) {
            continue;
        }
        check.files += 1;
        let size = file.metadata().map(|m| m.len()).unwrap_or(0);
        match root.join(relative).metadata() {
            Err(_) => check.missing.push(name),
            Ok(copy) if copy.len() != size => check.different.push(name),
            Ok(_) => {}
        }
    }
    Ok(check)
}
