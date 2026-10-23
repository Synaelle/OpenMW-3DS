//! The setup app's window: two folders (Morrowind, the output) and Prepare, then the progress, then the steps to
//! finish on the 3DS. The page (ui/) asks this side for everything through commands; the preparation runs on its
//! own thread and reports through events.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use serde::Serialize;
use setup_core::log::Log;
use setup_core::prepare::{self, Options, Progress};
use setup_core::{detect, space, update, verify, Error};
use tauri::{AppHandle, Emitter, Manager, State};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_opener::OpenerExt;

const VERSION: &str = env!("CARGO_PKG_VERSION");

/// The name of the folder made inside an output folder the player picks (the tester: never straight into the
/// folder picked).
const OUTPUT_FOLDER: &str = "OpenMW-3DS";

#[derive(Default)]
struct App {
    cancel: Arc<AtomicBool>,
    working: Mutex<bool>,
}

/// The port's files: inside the app (a release: macOS's bundle, the AppImage), else next to the program (Windows,
/// a build).
fn port_folder(app: &AppHandle) -> PathBuf {
    if let Ok(resources) = app.path().resource_dir() {
        let port = dunce::simplified(&resources).join("port");
        if port.is_dir() {
            return port;
        }
    }
    detect::program_folder().join("port")
}

/// What the app remembers between runs: the last output folder prepared (the next run offers to update it).
fn settings_file(app: &AppHandle) -> Option<PathBuf> {
    app.path().app_config_dir().ok().map(|dir| dir.join("last-output.txt"))
}

fn remembered_output(app: &AppHandle) -> Option<PathBuf> {
    let text = std::fs::read_to_string(settings_file(app)?).ok()?;
    let path = PathBuf::from(text.trim());
    prepare::installed_version(&path).is_some().then_some(path)
}

fn remember_output(app: &AppHandle, output: &Path) {
    if let Some(file) = settings_file(app) {
        if let Some(dir) = file.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let _ = std::fs::write(file, output.to_string_lossy().as_bytes());
    }
}

/// Where the files go for a folder the player picked: an install's own folder or the port folder as they are,
/// otherwise an OpenMW-3DS folder made inside.
fn effective_output(app: &AppHandle, chosen: &Path) -> PathBuf {
    let is_port = dunce::canonicalize(chosen).ok().is_some_and(|c| Some(c) == dunce::canonicalize(port_folder(app)).ok());
    let named = chosen.file_name().is_some_and(|name| name.to_string_lossy().starts_with(OUTPUT_FOLDER));
    if chosen.as_os_str().is_empty() || is_port || named || prepare::installed_version(chosen).is_some() {
        chosen.to_path_buf()
    } else {
        chosen.join(OUTPUT_FOLDER)
    }
}

#[derive(Serialize)]
struct Startup {
    version: &'static str,
    morrowind: Option<String>,
    output: String,
    port_found: bool,
}

#[tauri::command]
fn startup(app: AppHandle) -> Startup {
    let port = port_folder(&app);
    let output = remembered_output(&app).unwrap_or_else(|| port.clone());
    Startup {
        version: VERSION,
        morrowind: detect::find_morrowind().first().map(|p| p.display().to_string()),
        output: output.display().to_string(),
        port_found: port.is_dir(),
    }
}

/// Shows the window once the page has drawn (no blank window first).
#[tauri::command]
fn shown(window: tauri::WebviewWindow) {
    let _ = window.show();
    let _ = window.set_focus();
}

#[derive(Serialize)]
struct MorrowindInfo {
    problems: Vec<String>,
    language: Option<&'static str>,
    expansions: Vec<String>,
}

#[tauri::command]
fn inspect_morrowind(path: String) -> MorrowindInfo {
    let path = PathBuf::from(path.trim());
    if path.as_os_str().is_empty() {
        return MorrowindInfo {
            problems: vec!["Choose the folder Morrowind is installed in.".to_string()],
            language: None,
            expansions: Vec::new(),
        };
    }
    let problems = prepare::check_morrowind(&path);
    let edition = prepare::edition(&path);
    MorrowindInfo { problems, language: edition.language, expansions: edition.expansions }
}

#[derive(Serialize)]
struct OutputInfo {
    effective: String,
    installed: Option<String>,
    install_size: String,
    to_write: String,
    available: Option<String>,
    fits: bool,
}

/// The output folder: where the files go, the install already there, the space it all takes.
#[tauri::command]
async fn inspect_output(app: AppHandle, morrowind: String, output: String) -> OutputInfo {
    let effective = effective_output(&app, Path::new(output.trim()));
    let port = port_folder(&app);
    let morrowind = PathBuf::from(morrowind.trim());
    tauri::async_runtime::spawn_blocking(move || {
        let installed = prepare::installed_version(&effective);
        let plan = if prepare::check_morrowind(&morrowind).is_empty() {
            space::plan(&morrowind, &effective, &port)
        } else {
            space::SpacePlan { available: space::available_space(&effective), ..Default::default() }
        };
        OutputInfo {
            effective: effective.display().to_string(),
            installed,
            install_size: space::describe(plan.install_size),
            to_write: space::describe(plan.to_write),
            available: plan.available.map(space::describe),
            fits: plan.fits(),
        }
    })
    .await
    .unwrap_or(OutputInfo {
        effective: String::new(),
        installed: None,
        install_size: String::new(),
        to_write: String::new(),
        available: None,
        fits: true,
    })
}

/// The system's folder picker, opened at `start` (or its closest existing parent). None when cancelled.
#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: String) -> Option<String> {
    let mut folder = Some(PathBuf::from(start.trim()));
    while let Some(path) = &folder {
        if path.is_dir() {
            break;
        }
        folder = path.parent().map(Path::to_path_buf).filter(|p| !p.as_os_str().is_empty());
    }
    let mut dialog = app.dialog().file().set_title(title);
    if let Some(folder) = folder {
        dialog = dialog.set_directory(folder);
    }
    let picked = dialog.blocking_pick_folder()?;
    picked.into_path().ok().map(|p| p.display().to_string())
}

/// The picked output folder as the files will go there (an OpenMW-3DS folder inside).
#[tauri::command]
fn output_for(app: AppHandle, picked: String) -> String {
    effective_output(&app, Path::new(&picked)).display().to_string()
}

#[derive(Clone, Serialize)]
struct ProgressEvent {
    step: String,
    fraction: f64,
}

#[derive(Clone, Serialize)]
struct Finished {
    outcome: &'static str, // "done", "failed" or "cancelled"
    error: String,
    output: String,
    skipped: Vec<String>,
    size: String,
    log: Option<String>,
}

#[tauri::command]
fn start_prepare(app: AppHandle, state: State<'_, App>, morrowind: String, output: String) -> Result<String, String> {
    let mut working = state.working.lock().unwrap();
    if *working {
        return Err("Already preparing.".to_string());
    }
    let port = port_folder(&app);
    if !port.is_dir() {
        return Err("The port's files are missing: keep the \"port\" folder next to this program.".to_string());
    }
    let output = effective_output(&app, Path::new(output.trim()));
    *working = true;
    state.cancel.store(false, Ordering::Relaxed);
    let cancel = state.cancel.clone();
    let options =
        Options { morrowind: PathBuf::from(morrowind.trim()), output: output.clone(), port, version: VERSION.to_string() };
    let handle = app.clone();
    std::thread::spawn(move || {
        let mut log = Log::new();
        let emit = |p: Progress| {
            let _ = handle.emit("progress", ProgressEvent { step: p.step, fraction: p.fraction });
        };
        let result = prepare::prepare(&options, Some(&cancel), &emit, &mut log);
        let log_path = log.path().map(|p| p.display().to_string());
        let finished = match result {
            Ok(report) => {
                remember_output(&handle, &options.output);
                Finished {
                    outcome: "done",
                    error: String::new(),
                    output: options.output.display().to_string(),
                    skipped: report.skipped,
                    size: space::describe(report.size),
                    log: log_path,
                }
            }
            Err(error) => {
                if let Error::Failed(message) = &error {
                    log.line(&format!("Failed: {message}"));
                } else {
                    log.line("Cancelled");
                }
                Finished {
                    outcome: if error == Error::Cancelled { "cancelled" } else { "failed" },
                    error: error.to_string(),
                    output: options.output.display().to_string(),
                    skipped: Vec::new(),
                    size: String::new(),
                    log: log_path,
                }
            }
        };
        *handle.state::<App>().working.lock().unwrap() = false;
        let _ = handle.emit("finished", finished);
    });
    Ok(output.display().to_string())
}

#[tauri::command]
fn cancel(state: State<'_, App>) {
    state.cancel.store(true, Ordering::Relaxed);
}

#[derive(Serialize)]
struct SdCheck {
    root: String,
    files: usize,
    missing: Vec<String>,
    different: Vec<String>,
    error: Option<String>,
}

/// Picks the SD card's folder, then compares the prepared folder with it.
#[tauri::command]
async fn check_sd(app: AppHandle, output: String) -> Option<SdCheck> {
    let picked = app.dialog().file().set_title("Choose your SD card").blocking_pick_folder()?;
    let picked = picked.into_path().ok()?;
    let output = PathBuf::from(output);
    let check = tauri::async_runtime::spawn_blocking(move || verify::check_copy(&output, &picked)).await.ok()?;
    Some(match check {
        Ok(check) => SdCheck {
            root: check.root.display().to_string(),
            files: check.files,
            missing: check.missing,
            different: check.different,
            error: None,
        },
        Err(error) => SdCheck {
            root: String::new(),
            files: 0,
            missing: Vec::new(),
            different: Vec::new(),
            error: Some(error.to_string()),
        },
    })
}

#[tauri::command]
fn open_folder(app: AppHandle, path: String) {
    let _ = app.opener().open_path(path, None::<&str>);
}

#[tauri::command]
fn open_url(app: AppHandle, url: String) {
    if url.starts_with("https://") {
        let _ = app.opener().open_url(url, None::<&str>);
    }
}

/// A newer release on GitHub than this one, if there is (None without a connection).
#[tauri::command]
async fn newer_version() -> Option<String> {
    let latest = tauri::async_runtime::spawn_blocking(update::latest_release_version).await.ok()??;
    update::older_than(VERSION, &latest).then_some(latest)
}

#[derive(Serialize)]
struct Qr {
    url: String,
    svg: String,
}

/// The QR code FBI scans to install this release's CIA.
#[tauri::command]
fn cia_qr() -> Option<Qr> {
    let url = update::cia_url(VERSION);
    let code = qrcode::QrCode::new(url.as_bytes()).ok()?;
    let svg = code
        .render::<qrcode::render::svg::Color>()
        .quiet_zone(true)
        .dark_color(qrcode::render::svg::Color("#000000"))
        .light_color(qrcode::render::svg::Color("#ffffff"))
        .build();
    Some(Qr { url, svg })
}

#[tauri::command]
fn releases_page() -> String {
    update::releases_page()
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(App::default())
        .invoke_handler(tauri::generate_handler![
            startup,
            shown,
            inspect_morrowind,
            inspect_output,
            pick_folder,
            output_for,
            start_prepare,
            cancel,
            check_sd,
            open_folder,
            open_url,
            newer_version,
            cia_qr,
            releases_page,
        ])
        .on_window_event(|window, event| {
            // Closing while preparing stops the preparation (its thread ends at the next step).
            if let tauri::WindowEvent::CloseRequested { .. } = event {
                window.state::<App>().cancel.store(true, Ordering::Relaxed);
            }
        })
        .run(tauri::generate_context!())
        .expect("the window couldn't open");
}
