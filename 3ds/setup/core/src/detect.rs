//! Finding Morrowind on this computer, and the program's own folders.

use std::path::{Path, PathBuf};

use crate::prepare::check_morrowind;

/// A string value from the registry, in both registry views (32-bit installers write to WOW6432Node).
#[cfg(windows)]
fn registry_path(root: winreg::HKEY, key: &str, value: &str) -> Option<PathBuf> {
    use winreg::RegKey;
    use winreg::enums::{KEY_READ, KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    for view in [KEY_WOW64_32KEY, KEY_WOW64_64KEY] {
        if let Ok(opened) = RegKey::predef(root).open_subkey_with_flags(key, KEY_READ | view) {
            if let Ok(text) = opened.get_value::<String, _>(value) {
                return Some(PathBuf::from(text));
            }
        }
    }
    None
}

/// Steam's library folders (steamapps/libraryfolders.vdf: "path" entries), Steam's own folder first.
fn steam_libraries(steam: &Path) -> Vec<PathBuf> {
    let mut libraries = vec![steam.to_path_buf()];
    let Ok(text) = std::fs::read_to_string(steam.join("steamapps").join("libraryfolders.vdf")) else {
        return libraries;
    };
    // "path", spaces, then the quoted folder (VDF escapes backslashes).
    let mut rest = text.as_str();
    while let Some(at) = rest.find("\"path\"") {
        rest = &rest[at + 6..];
        let value = rest.trim_start();
        if let Some(quoted) = value.strip_prefix('"') {
            if let Some(end) = quoted.find('"') {
                if end > 0 {
                    libraries.push(PathBuf::from(quoted[..end].replace("\\\\", "\\")));
                }
            }
        }
    }
    libraries
}

/// Morrowind installations found on this computer, the usable ones (check_morrowind finds no problem) first:
/// Steam's libraries, GOG and the original installer (their registry entries on Windows), then the usual folders.
pub fn find_morrowind() -> Vec<PathBuf> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    #[cfg(windows)]
    {
        use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
        let steam = registry_path(HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath")
            .or_else(|| registry_path(HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"));
        if let Some(steam) = steam {
            for library in steam_libraries(&steam) {
                candidates.push(library.join("steamapps").join("common").join("Morrowind"));
            }
        }
        for (key, value) in [
            (r"SOFTWARE\GOG.com\Games\1435828767", "path"),
            (r"SOFTWARE\GOG.com\Games\1440163901", "path"),
            (r"SOFTWARE\Bethesda Softworks\Morrowind", "Installed Path"),
        ] {
            candidates.extend(registry_path(HKEY_LOCAL_MACHINE, key, value));
        }
        for folder in [
            r"C:\Program Files (x86)\Steam\steamapps\common\Morrowind",
            r"C:\GOG Games\Morrowind",
            r"C:\Program Files (x86)\GOG Galaxy\Games\Morrowind",
            r"C:\Program Files (x86)\Bethesda Softworks\Morrowind",
        ] {
            candidates.push(PathBuf::from(folder));
        }
    }
    #[cfg(not(windows))]
    if let Some(home) = dirs::home_dir() {
        for steam in [
            home.join(".steam").join("steam"),
            home.join(".local").join("share").join("Steam"),
            home.join("Library").join("Application Support").join("Steam"),
        ] {
            for library in steam_libraries(&steam) {
                candidates.push(library.join("steamapps").join("common").join("Morrowind"));
            }
        }
        candidates.push(home.join("GOG Games").join("Morrowind"));
    }

    let mut found: Vec<PathBuf> = Vec::new();
    for candidate in candidates {
        if candidate.as_os_str().is_empty() || !candidate.is_dir() {
            continue;
        }
        let path = dunce::canonicalize(&candidate).unwrap_or(candidate);
        if !found.contains(&path) {
            found.push(path);
        }
    }
    // The usable ones first, in their order (a stable sort).
    found.sort_by_key(|path| !check_morrowind(path).is_empty());
    found
}

/// Where the output goes unless the player picks another folder: OpenMW-3DS on the desktop.
pub fn default_output() -> PathBuf {
    if let Some(desktop) = dirs::desktop_dir() {
        return desktop.join("OpenMW-3DS");
    }
    if let Some(home) = dirs::home_dir() {
        return home.join("Desktop").join("OpenMW-3DS");
    }
    std::env::current_dir().unwrap_or_default().join("OpenMW-3DS")
}

/// The folder of the running program, where the release puts the port's files ("port"). An AppImage runs from a
/// temporary mount: its folder is the .AppImage file's. A macOS app's is the folder holding the .app.
pub fn program_folder() -> PathBuf {
    if let Some(app_image) = std::env::var_os("APPIMAGE") {
        if let Some(parent) = Path::new(&app_image).parent() {
            return parent.to_path_buf();
        }
    }
    let Ok(exe) = std::env::current_exe() else {
        return std::env::current_dir().unwrap_or_default();
    };
    let exe = dunce::canonicalize(&exe).unwrap_or(exe);
    let folder = exe.parent().map(Path::to_path_buf).unwrap_or_default();
    // <folder>/X.app/Contents/MacOS/x
    if folder.ends_with("Contents/MacOS") {
        if let Some(bundle) = folder.parent().and_then(Path::parent) {
            if bundle.extension().is_some_and(|e| e == "app") {
                return bundle.parent().map(Path::to_path_buf).unwrap_or_default();
            }
        }
    }
    folder
}
