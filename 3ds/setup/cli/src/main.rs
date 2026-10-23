//! The setup app's command-line tool: the same preparation as the window, for scripts and testing.
//!
//!     openmw3ds-setup-cli --morrowind "<Morrowind folder>" --output <folder> --port <port files> [--version 0.1.1]
//!
//! Developer commands (the texture cooker alone, against the C++ and Python cookers' packs; the space plan; a
//! check of a copy):
//!
//!     openmw3ds-setup-cli cook --morrowind <folder> --port <port files> --out <pack> [--limit N] [--threads N] [--keys <file>]
//!     openmw3ds-setup-cli dump --morrowind <folder> --port <port files> --key <texture key> --out <folder>
//!     openmw3ds-setup-cli space --morrowind <folder> --output <folder> --port <port files>
//!     openmw3ds-setup-cli check --output <folder> --sd <SD card folder>

use std::collections::BTreeMap;
use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Mutex;
use std::time::Instant;

use setup_core::cooker::{self, CookOptions};
use setup_core::log::Log;
use setup_core::prepare::{self, Options, Progress};
use setup_core::{space, verify};

const VERSION: &str = env!("CARGO_PKG_VERSION");

fn usage() -> ExitCode {
    eprintln!(
        "usage: openmw3ds-setup-cli --morrowind <Morrowind folder> --output <folder> --port <port files> [--version <version>]\n       \
         openmw3ds-setup-cli cook --morrowind <folder> --port <port files> --out <pack> [--limit N] [--threads N] [--keys <file>]\n       \
         openmw3ds-setup-cli dump --morrowind <folder> --port <port files> --key <key> --out <folder>\n       \
         openmw3ds-setup-cli space --morrowind <folder> --output <folder> --port <port files>\n       \
         openmw3ds-setup-cli check --output <folder> --sd <SD card folder>"
    );
    ExitCode::from(1)
}

fn main() -> ExitCode {
    let mut args: Vec<std::ffi::OsString> = std::env::args_os().skip(1).collect();
    let command = match args.first() {
        Some(first) if !first.to_string_lossy().starts_with('-') => args.remove(0).to_string_lossy().into_owned(),
        _ => String::new(),
    };
    if args.len() % 2 != 0 {
        return usage();
    }
    let options: BTreeMap<String, PathBuf> = args
        .chunks(2)
        .map(|pair| (pair[0].to_string_lossy().into_owned(), PathBuf::from(&pair[1])))
        .collect();
    let get = |name: &str| options.get(name).cloned();
    let number = |name: &str| get(name).and_then(|v| v.to_string_lossy().parse::<usize>().ok()).unwrap_or(0);

    let result = match command.as_str() {
        "" => {
            let (Some(morrowind), Some(output), Some(port)) = (get("--morrowind"), get("--output"), get("--port")) else {
                return usage();
            };
            let version = get("--version").map_or(VERSION.to_string(), |v| v.to_string_lossy().into_owned());
            let last = Mutex::new((String::new(), -1i32));
            let print = |p: Progress| {
                let percent = (p.fraction * 100.0) as i32;
                let mut last = last.lock().unwrap();
                if last.0 == p.step && last.1 == percent {
                    return;
                }
                *last = (p.step.clone(), percent);
                eprint!("\r{percent:3}%  {:<40}", p.step);
                if p.fraction >= 1.0 {
                    eprintln!();
                }
                let _ = std::io::stderr().flush();
            };
            let mut log = Log::new();
            prepare::prepare(&Options { morrowind, output, port, version }, None, &print, &mut log).map(|report| {
                if !report.skipped.is_empty() {
                    eprintln!("Kept from the earlier install: {}", report.skipped.join(", "));
                }
                eprintln!("{} files, {} in {:.1} s", report.files, space::describe(report.size), report.seconds);
            })
        }
        "cook" | "dump" => {
            let (Some(morrowind), Some(port), Some(out)) = (get("--morrowind"), get("--port"), get("--out")) else {
                return usage();
            };
            let cook = CookOptions {
                data_files: morrowind.join("Data Files"),
                archives: prepare::ARCHIVES.iter().map(|s| s.to_string()).collect(),
                engine_vfs: port.join("openmw").join("resources").join("vfs"),
                port_interface: port.join("openmw").join("ui3ds"),
                out: out.clone(),
                threads: number("--threads"),
                limit: number("--limit"),
                key_list: get("--keys"),
            };
            if command == "dump" {
                let Some(key) = get("--key") else {
                    return usage();
                };
                cooker::dump_texture_steps(&cook, &key.to_string_lossy(), &out)
            } else {
                let start = Instant::now();
                cooker::cook_textures(&cook, None, &|done, total| {
                    if done % 100 == 0 || done == total {
                        eprint!("\r  {done} / {total}");
                    }
                })
                .map(|summary| {
                    eprintln!(
                        "\n{} textures ({} in the UI atlas), {:.1} MB in {:.1} s",
                        summary.textures,
                        summary.atlas_images,
                        summary.bytes as f64 / 1048576.0,
                        start.elapsed().as_secs_f64()
                    );
                })
            }
        }
        "space" => {
            let (Some(morrowind), Some(output), Some(port)) = (get("--morrowind"), get("--output"), get("--port")) else {
                return usage();
            };
            let start = Instant::now();
            let plan = space::plan(&morrowind, &output, &port);
            println!(
                "install {} ({} bytes), to write {} ({} bytes), available {}, fits {} ({:.2} s)",
                space::describe(plan.install_size),
                plan.install_size,
                space::describe(plan.to_write),
                plan.to_write,
                plan.available.map_or("unknown".to_string(), space::describe),
                plan.fits(),
                start.elapsed().as_secs_f64()
            );
            Ok(())
        }
        "check" => {
            let (Some(output), Some(sd)) = (get("--output"), get("--sd")) else {
                return usage();
            };
            verify::check_copy(&output, &sd).map(|check| {
                println!(
                    "{} files compared in {}: {} missing, {} different",
                    check.files,
                    check.root.display(),
                    check.missing.len(),
                    check.different.len()
                );
                for name in check.missing.iter().chain(&check.different).take(20) {
                    println!("  {name}");
                }
            })
        }
        _ => return usage(),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("\n{e}");
            ExitCode::from(2)
        }
    }
}
