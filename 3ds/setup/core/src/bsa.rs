//! Morrowind's archives (TES3 BSA): the sound archive the setup packs.

use std::fs::File;
use std::io::{BufWriter, Read, Write};
use std::path::{Path, PathBuf};

use crate::{fail, io_error, Result};

/// The TES3 BSA name hash (components/bsa/bsafile.cpp) of a lower-case path with backslashes.
pub fn tes3_hash(name: &[u8]) -> (u32, u32) {
    let half = name.len() / 2;
    let (mut low, mut high) = (0u32, 0u32);
    let mut shift = 0u32;
    for &c in &name[..half] {
        low ^= u32::from(c) << (shift & 0x1F);
        shift += 8;
    }
    shift = 0;
    for &c in &name[half..] {
        let temp = u32::from(c) << (shift & 0x1F);
        high ^= temp;
        high = high.rotate_right(temp & 0x1F);
        shift += 8;
    }
    (low, high)
}

/// Every file under a folder, depth first.
pub(crate) fn walk_files(root: &Path, out: &mut Vec<PathBuf>) -> std::io::Result<()> {
    let mut entries: Vec<_> = std::fs::read_dir(root)?.collect::<std::io::Result<_>>()?;
    entries.sort_by_key(|e| e.file_name());
    for entry in entries {
        let kind = entry.file_type()?;
        if kind.is_dir() {
            walk_files(&entry.path(), out)?;
        } else if kind.is_file() {
            out.push(entry.path());
        }
    }
    Ok(())
}

/// A file's name in an archive: relative to Data Files, lower case, backslashes.
fn archive_name(path: &Path, data_files: &Path) -> String {
    let relative = path.strip_prefix(data_files).unwrap_or(path);
    let parts: Vec<String> = relative.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect();
    parts.join("\\").to_ascii_lowercase()
}

pub struct SoundFile {
    pub name: String,
    pub path: PathBuf,
    pub size: u64,
}

/// The .wav and .mp3 files of `folders` (under `data_files`), as the sound archive holds them.
pub fn sound_files(data_files: &Path, folders: &[&str]) -> Result<Vec<SoundFile>> {
    let mut files = Vec::new();
    for folder in folders {
        let root = data_files.join(folder);
        if !root.is_dir() {
            return fail(format!("no {folder} folder in {}", data_files.display()));
        }
        let mut paths = Vec::new();
        walk_files(&root, &mut paths).map_err(|e| io_error("can't read", &root, e))?;
        for path in paths {
            let extension = path.extension().map(|e| e.to_string_lossy().to_ascii_lowercase());
            if !matches!(extension.as_deref(), Some("wav" | "mp3")) {
                continue;
            }
            let size = path.metadata().map_err(|e| io_error("can't read", &path, e))?.len();
            if size >= 1 << 32 {
                return fail(format!("{} is larger than 4 GB", path.display()));
            }
            files.push(SoundFile { name: archive_name(&path, data_files), path, size });
        }
    }
    Ok(files)
}

/// The size of the archive these files make.
pub fn archive_size(files: &[SoundFile]) -> u64 {
    let names: u64 = files.iter().map(|f| f.name.len() as u64 + 1).sum();
    let data: u64 = files.iter().map(|f| f.size).sum();
    12 + 12 * files.len() as u64 + names + 8 * files.len() as u64 + data
}

pub struct BsaSummary {
    pub files: usize,
    pub data_bytes: u64,
}

/// Packs the files into one TES3 BSA, as OpenMW reads it with fallback-archive=: the sound3ds.bsa the port loads
/// instead of ~7,200 loose files (indexing them took 12.8 s on the console). Written to `out` through a temporary
/// file. `progress` gets the bytes written and the total, and stops the packing when it returns an error.
pub fn pack_archive(
    mut files: Vec<SoundFile>,
    out: &Path,
    progress: &mut dyn FnMut(u64, u64) -> Result<()>,
) -> Result<BsaSummary> {
    let hashes: Vec<(u32, u32)> = files.iter().map(|f| tes3_hash(f.name.as_bytes())).collect();
    let mut order: Vec<usize> = (0..files.len()).collect();
    order.sort_by_key(|&i| hashes[i]);
    let hashes: Vec<(u32, u32)> = order.iter().map(|&i| hashes[i]).collect();
    let mut sorted: Vec<Option<SoundFile>> = files.drain(..).map(Some).collect();
    let files: Vec<SoundFile> = order.iter().map(|&i| sorted[i].take().unwrap()).collect();

    if archive_size(&files) >= 1 << 32 {
        return fail("the sound archive would be larger than 4 GB");
    }
    let names_size: u32 = files.iter().map(|f| f.name.len() as u32 + 1).sum();
    let total: u64 = files.iter().map(|f| f.size).sum();
    let count = files.len() as u32;
    let dir_size = 12 * count + names_size;

    if let Some(parent) = out.parent() {
        std::fs::create_dir_all(parent).map_err(|e| io_error("can't create", parent, e))?;
    }
    let part = PathBuf::from(format!("{}.part", out.display()));
    let write_error = |e| io_error("can't write", &part, e);
    {
        let mut file = BufWriter::with_capacity(1 << 20, File::create(&part).map_err(write_error)?);
        let mut head = Vec::new();
        for value in [0x100, dir_size, count] {
            head.extend_from_slice(&u32::to_le_bytes(value));
        }
        let mut offset = 0u32;
        for f in &files {
            head.extend_from_slice(&(f.size as u32).to_le_bytes());
            head.extend_from_slice(&offset.to_le_bytes());
            offset += f.size as u32;
        }
        let mut name_offset = 0u32;
        for f in &files {
            head.extend_from_slice(&name_offset.to_le_bytes());
            name_offset += f.name.len() as u32 + 1;
        }
        for f in &files {
            head.extend_from_slice(f.name.as_bytes());
            head.push(0);
        }
        for (low, high) in &hashes {
            head.extend_from_slice(&low.to_le_bytes());
            head.extend_from_slice(&high.to_le_bytes());
        }
        file.write_all(&head).map_err(write_error)?;
        let mut buffer = Vec::new();
        let mut written = 0u64;
        for f in &files {
            buffer.clear();
            let read = File::open(&f.path).and_then(|mut input| input.read_to_end(&mut buffer));
            if read.is_err() || buffer.len() as u64 != f.size {
                return fail(format!("{} couldn't be read whole", f.path.display()));
            }
            file.write_all(&buffer).map_err(write_error)?;
            written += f.size;
            progress(written, total)?;
        }
        file.flush().map_err(write_error)?;
    }
    std::fs::rename(&part, out).map_err(|e| io_error("can't write", out, e))?;
    Ok(BsaSummary { files: files.len(), data_bytes: total })
}
