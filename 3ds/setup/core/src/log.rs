//! The setup's log (openmw/logs/setup.log in the install): what was found and done, for bug reports.

use std::fs::File;
use std::io::Write;
use std::path::{Path, PathBuf};

/// Lines with the time, written as they come. Failing to write the log never stops the preparation.
#[derive(Default)]
pub struct Log {
    file: Option<File>,
    path: Option<PathBuf>,
    /// The lines written before the file was opened (or without one), kept for it.
    pending: Vec<String>,
}

impl Log {
    pub fn new() -> Self {
        Log::default()
    }

    /// Starts the file at `path` (replacing an earlier run's), with the lines logged so far.
    pub fn open(&mut self, path: &Path) {
        self.file = File::create(path).ok();
        self.path = Some(path.to_path_buf());
        for line in std::mem::take(&mut self.pending) {
            self.write(&line);
        }
    }

    pub fn path(&self) -> Option<&Path> {
        self.path.as_deref()
    }

    pub fn line(&mut self, text: &str) {
        let line = format!("{} {text}", chrono::Local::now().format("%d/%m/%Y %H:%M:%S"));
        if self.file.is_some() {
            self.write(&line);
        } else {
            self.pending.push(line);
        }
    }

    fn write(&mut self, line: &str) {
        if let Some(file) = &mut self.file {
            let _ = writeln!(file, "{line}");
            let _ = file.flush();
        }
    }
}
