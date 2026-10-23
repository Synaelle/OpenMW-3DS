//! The setup app's preparation, without an interface: checking the Morrowind folder, finding it (Steam, GOG, the
//! original installer), `openmw.cfg` and its fallback values, `sound3ds.bsa`, the texture cooker, the space it all
//! needs, checking a copy on the SD card. The window and the command-line tool are built on it.

pub mod bsa;
pub mod cooker;
pub mod detect;
pub mod esm;
pub mod fallbacks;
pub mod log;
pub mod prepare;
pub mod space;
pub mod text;
pub mod update;
pub mod verify;

use std::fmt;

/// Why a step stopped: the player cancelled it, or something failed (the message is for the player).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    Cancelled,
    Failed(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Cancelled => f.write_str("Cancelled"),
            Error::Failed(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// A failure with a message.
pub fn fail<T>(message: impl Into<String>) -> Result<T> {
    Err(Error::Failed(message.into()))
}

/// What a path's file operation failed with, for the player: "can't write <path> (<reason>)".
pub(crate) fn io_error(what: &str, path: &std::path::Path, error: std::io::Error) -> Error {
    Error::Failed(format!("{what} {} ({error})", path.display()))
}
