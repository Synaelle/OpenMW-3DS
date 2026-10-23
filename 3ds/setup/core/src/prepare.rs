//! The preparation: the folder players copy to their SD card, from their own Morrowind installation. The game's
//! files are only read.
//!
//! Preparing a folder that already has an install updates it: the steps whose sources haven't changed since (the
//! sounds, the game's files, the textures) are skipped, which leaves mostly the port's own files to copy.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

use crate::bsa::{self, walk_files};
use crate::cooker::{self, CookOptions};
use crate::esm::{esm_language, locale_for};
use crate::fallbacks::read_fallbacks;
use crate::log::Log;
use crate::{fail, io_error, Error, Result};

/// The content the port loads: Morrowind alone (the expansions come with 0.3.0).
pub const CONTENT: &[&str] = &["Morrowind.esm"];
pub const ARCHIVES: &[&str] = &["Morrowind.bsa"];
/// The expansions, found but not loaded yet.
pub const EXPANSIONS: &[&str] = &["Tribunal.esm", "Bloodmoon.esm"];
/// Folders the game would create on first use: creating a folder takes ~1.5 s on the console's SD card.
const EMPTY_FOLDERS: &[&str] = &["cache", "saves", "screenshots", "logs"];
const SOUND_FOLDERS: &[&str] = &["Sound", "Music"];

/// What the setup remembers about an install it prepared (openmw/setup-state.txt): its version and what each
/// skippable step was made from.
pub const STATE_FILE: &str = "setup-state.txt";

// Each step's share of the whole, roughly its time (the textures take about a minute).
const SHARE_CHECK: f64 = 0.01;
const SHARE_PORT: f64 = 0.03;
const SHARE_CONFIG: f64 = 0.01;
const SHARE_SOUND: f64 = 0.15;
const SHARE_DATA: f64 = 0.15;
const SHARE_TEXTURES: f64 = 0.65;

pub struct Options {
    /// The Morrowind installation (Morrowind.ini and Data Files inside).
    pub morrowind: PathBuf,
    /// What the player copies to the root of the 3DS's SD card.
    pub output: PathBuf,
    /// The port's own files from the release, laid out as on the SD card.
    pub port: PathBuf,
    /// The release, written to openmw/VERSION.
    pub version: String,
}

#[derive(Clone, Debug, Default)]
pub struct Progress {
    /// What is being done, for the player.
    pub step: String,
    /// Of the whole preparation.
    pub fraction: f64,
}

/// What a preparation did.
#[derive(Clone, Debug, Default)]
pub struct Report {
    /// The steps skipped because nothing they read had changed ("the sounds", "the textures"...).
    pub skipped: Vec<String>,
    /// The install's size (what the SD card needs) and its file count.
    pub size: u64,
    pub files: usize,
    pub seconds: f64,
}

/// The problems found in the Morrowind folder (empty: it can be used).
pub fn check_morrowind(morrowind: &Path) -> Vec<String> {
    let mut problems = Vec::new();
    let data_files = morrowind.join("Data Files");
    if !data_files.is_dir() {
        problems.push(format!("No \"Data Files\" folder in {}.", morrowind.display()));
        return problems;
    }
    for file in CONTENT.iter().chain(ARCHIVES) {
        if !data_files.join(file).is_file() {
            problems.push(format!("{file} is missing from Data Files."));
        }
    }
    for folder in SOUND_FOLDERS {
        if !data_files.join(folder).is_dir() {
            problems.push(format!("The {folder} folder is missing from Data Files."));
        }
    }
    if !morrowind.join("Morrowind.ini").is_file() {
        problems.push("Morrowind.ini is missing.".to_string());
    }
    // English, French and German for now (the others later: Polish and Russian need other encodings).
    if problems.is_empty() {
        match esm_language(&data_files.join("Morrowind.esm")) {
            None => problems.push(
                "This Morrowind's language isn't recognised: this version supports the English, French and German \
                 games."
                    .to_string(),
            ),
            Some(language) if locale_for(language).is_none() => problems.push(format!(
                "This Morrowind is in {language}: this version supports the English, French and German games. \
                 {language} will be supported in a later version."
            )),
            Some(_) => {}
        }
    }
    problems
}

/// The Morrowind edition found: its language and the expansions installed with it.
#[derive(Clone, Debug, Default)]
pub struct Edition {
    pub language: Option<&'static str>,
    pub expansions: Vec<String>,
}

pub fn edition(morrowind: &Path) -> Edition {
    let data_files = morrowind.join("Data Files");
    Edition {
        language: esm_language(&data_files.join("Morrowind.esm")),
        expansions: EXPANSIONS
            .iter()
            .filter(|file| data_files.join(file).is_file())
            .map(|file| file.trim_end_matches(".esm").to_string())
            .collect(),
    }
}

/// The install's state file: "key value" lines.
pub fn read_state(output: &Path) -> BTreeMap<String, String> {
    let text = std::fs::read_to_string(output.join("openmw").join(STATE_FILE)).unwrap_or_default();
    text.lines()
        .filter_map(|line| line.split_once(' '))
        .map(|(key, value)| (key.to_string(), value.trim().to_string()))
        .collect()
}

fn write_state(output: &Path, state: &BTreeMap<String, String>) -> Result<()> {
    let path = output.join("openmw").join(STATE_FILE);
    let text: String = state.iter().map(|(key, value)| format!("{key} {value}\n")).collect();
    std::fs::write(&path, text).map_err(|e| io_error("can't write", &path, e))
}

/// The version of the install in a folder (its openmw/VERSION), if it has one.
pub fn installed_version(output: &Path) -> Option<String> {
    let text = std::fs::read_to_string(output.join("openmw").join("VERSION")).ok()?;
    let version = text.trim();
    (!version.is_empty()).then(|| version.to_string())
}

fn write_lines(path: &Path, lines: &[String]) -> Result<()> {
    let text: String = lines.iter().map(|line| format!("{line}\n")).collect();
    std::fs::write(path, text).map_err(|e| io_error("can't write", path, e))
}

/// Sets [General] preferred locales in a settings.cfg, keeping the rest as it is.
fn set_preferred_locales(path: &Path, locales: &str) -> Result<()> {
    let text = std::fs::read(path).unwrap_or_default();
    let mut lines: Vec<String> = String::from_utf8_lossy(&text)
        .split('\n')
        .map(|line| line.strip_suffix('\r').unwrap_or(line).to_string())
        .filter(|line| !line.starts_with("preferred locales"))
        .collect();
    // split gives an empty last line for a file ending with a line end (getline doesn't).
    if lines.last().is_some_and(String::is_empty) {
        lines.pop();
    }
    let setting = format!("preferred locales = {locales}");
    match lines.iter().position(|line| line == "[General]") {
        Some(i) => lines.insert(i + 1, setting),
        None => lines.extend([String::new(), "[General]".to_string(), setting]),
    }
    write_lines(path, &lines)
}

/// Whether a copy of `from` is already at `to`: the same size and modification time (copies get the source's).
fn same_file(from: &Path, to: &Path) -> bool {
    match (from.metadata(), to.metadata()) {
        (Ok(a), Ok(b)) => a.len() == b.len() && a.modified().ok().is_some() && a.modified().ok() == b.modified().ok(),
        _ => false,
    }
}

/// Copies through a temporary file, with a progress callback in bytes. The copy gets the source's modification
/// time (same_file then knows it).
fn copy_file(from: &Path, to: &Path, done: &mut dyn FnMut(u64) -> Result<()>) -> Result<()> {
    let mut input = File::open(from).map_err(|e| io_error("can't read", from, e))?;
    let modified = input.metadata().and_then(|m| m.modified()).ok();
    let part = PathBuf::from(format!("{}.part", to.display()));
    {
        let write_error = |e| io_error("can't write", &part, e);
        let mut out = File::create(&part).map_err(write_error)?;
        let mut buffer = vec![0u8; 4 << 20];
        let mut copied = 0u64;
        loop {
            let got = input.read(&mut buffer).map_err(|e| io_error("can't read", from, e))?;
            if got == 0 {
                break;
            }
            out.write_all(&buffer[..got]).map_err(write_error)?;
            copied += got as u64;
            done(copied)?;
        }
        out.flush().map_err(write_error)?;
        if let Some(modified) = modified {
            let _ = out.set_modified(modified);
        }
    }
    std::fs::rename(&part, to).map_err(|e| io_error("can't write", to, e))
}

/// Copies a folder's files into another, merging with what's there; the files already there (same_file) are left.
/// Returns how many were copied.
fn copy_tree(from: &Path, to: &Path) -> Result<usize> {
    let mut files = Vec::new();
    walk_files(from, &mut files).map_err(|e| io_error("can't read", from, e))?;
    let mut copied = 0;
    for file in files {
        let target = to.join(file.strip_prefix(from).unwrap_or(&file));
        if same_file(&file, &target) {
            continue;
        }
        if let Some(parent) = target.parent() {
            std::fs::create_dir_all(parent).map_err(|e| io_error("can't create", parent, e))?;
        }
        copy_file(&file, &target, &mut |_| Ok(()))?;
        copied += 1;
    }
    Ok(copied)
}

/// The fingerprint of the sound archive's sources: every file's name, size and modification time.
pub(crate) fn sound_fingerprint(files: &[bsa::SoundFile]) -> String {
    let mut text = String::new();
    for f in files {
        text += &format!("{} {}\n", f.name, cooker::file_stamp(&f.path));
    }
    format!("{:016x}", fnv1a64(&text))
}

pub(crate) fn fnv1a64(text: &str) -> u64 {
    let mut h = 0xCBF29CE484222325u64;
    for &c in text.as_bytes() {
        h ^= u64::from(c);
        h = h.wrapping_mul(0x100000001B3);
    }
    h
}

/// How the cooker is set up for an install.
pub(crate) fn cook_options(morrowind: &Path, port: &Path, out: PathBuf) -> CookOptions {
    CookOptions {
        data_files: morrowind.join("Data Files"),
        archives: ARCHIVES.iter().map(|s| s.to_string()).collect(),
        engine_vfs: port.join("openmw").join("resources").join("vfs"),
        port_interface: port.join("openmw").join("ui3ds"),
        out,
        threads: 0,
        limit: 0,
        key_list: None,
    }
}

/// The size and file count of a folder.
pub fn folder_size(folder: &Path) -> (u64, usize) {
    let mut files = Vec::new();
    if walk_files(folder, &mut files).is_err() {
        return (0, 0);
    }
    let size = files.iter().filter_map(|f| f.metadata().ok()).map(|m| m.len()).sum();
    (size, files.len())
}

/// Prepares the output folder. Fails with a message for the player, or Cancelled when `cancel` is set (the output
/// folder is then incomplete: preparing it again finishes it). `log` gets what was done and how long it took.
pub fn prepare(
    options: &Options,
    cancel: Option<&AtomicBool>,
    progress: &(dyn Fn(Progress) + Sync),
    log: &mut Log,
) -> Result<Report> {
    let started = Instant::now();
    let cancelled = || cancel.is_some_and(|c| c.load(Ordering::Relaxed));
    let mut base = 0.0;
    let report = |step: &str, within: f64, share: f64, base: f64| -> Result<()> {
        if cancelled() {
            return Err(Error::Cancelled);
        }
        progress(Progress { step: step.to_string(), fraction: base + share * within });
        Ok(())
    };
    let mut skipped = Vec::new();

    report("Checking the Morrowind folder", 0.0, SHARE_CHECK, base)?;
    if let Some(problem) = check_morrowind(&options.morrowind).into_iter().next() {
        return fail(problem);
    }
    let data_files = options.morrowind.join("Data Files");
    let openmw = options.output.join("openmw");
    for folder in EMPTY_FOLDERS.iter().chain(&["config", "data", "cooked"]) {
        let path = openmw.join(folder);
        std::fs::create_dir_all(&path).map_err(|e| io_error("can't create", &path, e))?;
    }
    log.open(&openmw.join("logs").join("setup.log"));
    log.line(&format!("OpenMW for New 3DS setup {} on {}", options.version, std::env::consts::OS));
    log.line(&format!("Morrowind: {}", options.morrowind.display()));
    let edition = edition(&options.morrowind);
    log.line(&format!(
        "Edition: {}{}",
        edition.language.unwrap_or("unknown"),
        if edition.expansions.is_empty() { String::new() } else { format!(", with {}", edition.expansions.join(" and ")) }
    ));
    log.line(&format!("Output: {}", options.output.display()));
    let previous = installed_version(&options.output);
    let mut state = read_state(&options.output);
    match &previous {
        Some(version) => log.line(&format!("Updating the install of {version} found there")),
        None => log.line("New install"),
    }
    base += SHARE_CHECK;

    // The port's own files (the engine's resources, the 3DS interface, settings, the CIA), as the release ships them.
    report("Copying the port's files", 0.0, SHARE_PORT, base)?;
    if !options.port.is_dir() {
        return fail(format!("The port's files are missing: {}", options.port.display()));
    }
    // The default output is the port folder itself (the game's files are added to it): nothing to copy then.
    let same_folder = dunce::canonicalize(&options.port).ok() == dunce::canonicalize(&options.output).ok();
    let step_start = Instant::now();
    if !same_folder {
        let copied = copy_tree(&options.port, &options.output)?;
        log.line(&format!("Port's files: {copied} copied ({:.1} s)", step_start.elapsed().as_secs_f64()));
    }
    // The language of OpenMW's and the port's own texts (the game's come from its files): the edition's, then
    // English for any text without a translation. Replaces what an earlier run wrote.
    let locale = edition.language.and_then(locale_for).unwrap_or("en");
    let locales = if locale == "en" { "en".to_string() } else { format!("{locale},en") };
    set_preferred_locales(&openmw.join("config").join("settings.cfg"), &locales)?;
    base += SHARE_PORT;

    // The player's openmw.cfg: the data folders, the content, the encoding and Morrowind.ini's fallback values.
    report("Reading Morrowind.ini", 0.0, SHARE_CONFIG, base)?;
    let mut cfg = vec![
        "# OpenMW 3DS user configuration (written by the setup app)".to_string(),
        "data=\"/openmw/data\"".to_string(),
        "data=\"/openmw/ui3ds\"".to_string(),
    ];
    cfg.extend(CONTENT.iter().map(|content| format!("content={content}")));
    cfg.extend(ARCHIVES.iter().map(|archive| format!("fallback-archive={archive}")));
    cfg.push("fallback-archive=sound3ds.bsa".to_string());
    cfg.push("encoding=win1252".to_string());
    cfg.push("# fallback values imported from Morrowind.ini by the setup app".to_string());
    let fallbacks = read_fallbacks(&options.morrowind.join("Morrowind.ini"))?;
    log.line(&format!("Morrowind.ini: {} fallback values", fallbacks.len()));
    cfg.extend(fallbacks);
    write_lines(&openmw.join("config").join("openmw.cfg"), &cfg)?;
    base += SHARE_CONFIG;

    // The sounds, in one archive.
    report("Packing the sounds", 0.0, SHARE_SOUND, base)?;
    let sounds = bsa::sound_files(&data_files, SOUND_FOLDERS)?;
    let sound_archive = openmw.join("data").join("sound3ds.bsa");
    let fingerprint = sound_fingerprint(&sounds);
    let expected_size = bsa::archive_size(&sounds);
    let sound_ready = state.get("sound") == Some(&fingerprint)
        && sound_archive.metadata().is_ok_and(|m| m.len() == expected_size);
    if sound_ready {
        skipped.push("the sounds".to_string());
        log.line("Sounds: unchanged, kept");
    } else {
        let step_start = Instant::now();
        let count = sounds.len();
        bsa::pack_archive(sounds, &sound_archive, &mut |done, total| {
            report("Packing the sounds", if total > 0 { done as f64 / total as f64 } else { 1.0 }, SHARE_SOUND, base)
        })?;
        state.insert("sound".to_string(), fingerprint);
        write_state(&options.output, &state)?;
        log.line(&format!(
            "Sounds: {count} files packed, {} MB ({:.1} s)",
            expected_size / 1_000_000,
            step_start.elapsed().as_secs_f64()
        ));
    }
    base += SHARE_SOUND;

    // The game's own files, copied as they are.
    let files: Vec<&str> = CONTENT.iter().chain(ARCHIVES).copied().collect();
    let total: u64 = files.iter().filter_map(|f| data_files.join(f).metadata().ok()).map(|m| m.len()).sum();
    let mut copied_before = 0u64;
    let mut kept = 0;
    let step_start = Instant::now();
    for file in &files {
        let (from, to) = (data_files.join(file), openmw.join("data").join(file));
        let size = from.metadata().map_err(|e| io_error("can't read", &from, e))?.len();
        if same_file(&from, &to) {
            kept += 1;
        } else {
            let step = format!("Copying {file}");
            copy_file(&from, &to, &mut |copied| {
                report(&step, (copied_before + copied) as f64 / total.max(1) as f64, SHARE_DATA, base)
            })?;
        }
        copied_before += size;
    }
    if kept == files.len() {
        skipped.push("the game's files".to_string());
        log.line("Game's files: unchanged, kept");
    } else {
        log.line(&format!("Game's files: copied ({:.1} s)", step_start.elapsed().as_secs_f64()));
    }
    base += SHARE_DATA;

    // The textures, cooked for the 3DS's GPU: the game's, OpenMW's own and the port's interface.
    report("Preparing the textures", 0.0, SHARE_TEXTURES, base)?;
    let pack = openmw.join("cooked").join("textures.o3tp");
    let cook = cook_options(&options.morrowind, &options.port, pack.clone());
    let fingerprint = cooker::source_fingerprint(&cook)?;
    let pack_size = pack.metadata().map(|m| m.len()).unwrap_or(0);
    let textures_ready = pack_size > 0 && state.get("textures") == Some(&format!("{fingerprint} {pack_size}"));
    if textures_ready {
        skipped.push("the textures".to_string());
        log.line("Textures: unchanged, kept");
    } else {
        let step_start = Instant::now();
        // Called from the cooking threads: only reports, cancelling goes through `cancel`.
        let summary = cooker::cook_textures(&cook, cancel, &|done, total| {
            let within = if total > 0 { done as f64 / total as f64 } else { 1.0 };
            progress(Progress { step: "Preparing the textures".to_string(), fraction: base + SHARE_TEXTURES * within });
        })?;
        state.insert("textures".to_string(), format!("{fingerprint} {}", summary.bytes));
        write_state(&options.output, &state)?;
        log.line(&format!(
            "Textures: {} cooked ({} in the interface atlas), {} MB ({:.1} s)",
            summary.textures,
            summary.atlas_images,
            summary.bytes / 1_000_000,
            step_start.elapsed().as_secs_f64()
        ));
    }

    write_lines(&openmw.join("VERSION"), std::slice::from_ref(&options.version))?;
    state.insert("version".to_string(), options.version.clone());
    write_state(&options.output, &state)?;
    let (size, count) = folder_size(&options.output);
    let seconds = started.elapsed().as_secs_f64();
    log.line(&format!("Done: {count} files, {} MB, in {seconds:.1} s", size / 1_000_000));
    progress(Progress { step: "Done".to_string(), fraction: 1.0 });
    Ok(Report { skipped, size, files: count, seconds })
}
