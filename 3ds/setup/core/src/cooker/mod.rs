//! The texture cooker: the port's tools/cook_textures.py in Rust, step for step (see its docstring for the why of
//! each choice). The same choices, sizes, formats and bytes, in one pack (format 4, read by the port's
//! texturepack.cpp).

mod bleed;
mod decode;
mod etc1;
mod etc1_tables;
mod image;
mod pica;
mod resample;

use std::collections::{BTreeMap, BTreeSet};
use std::fs::File;
use std::io::{BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::UNIX_EPOCH;

use self::bleed::bleed_transparent;
use self::decode::{dds_is_uncompressed, decode_image, tga_ignores_alpha};
use self::image::{cropped, flip_vertical, padded_edge, padded_edge_around, transposed, Image};
use self::pica::{encode_pica, PicaFormat};
use self::resample::{resize, Filter};
use crate::bsa::walk_files;
use crate::{fail, io_error, Error, Result};

const VERSION: u32 = 4;
const HEADER_SIZE: usize = 32;
const ENTRY_SIZE: usize = 32;
const DATA_ALIGN: usize = 128;

/// Changes when the cooker's output changes for the same files (a new choice, a fix): an install cooked by an
/// earlier revision is cooked again.
pub const REVISION: u32 = 1;

const FLAG_ALPHA: u8 = 1;
const FLAG_ALPHA_BINARY: u8 = 2;
const FLAG_PADDED: u8 = 4;
const FLAG_ATLAS: u8 = 8;
const FLAG_ATLAS_TRANSPOSED: u8 = 16;
const FLAG_ATLAS_HALVED: u8 = 32;

const ATLAS_HALVED_PREFIX: &str = "textures/menu_icon_";
const UI_PREFIXES: &[&str] = &["icons/", "bookart/", "textures/omw_", "textures/levelup/", "textures/birthsigns/"];
const UI_TEXTURE_NAMES: &[&str] = &["menu_", "tx_menu", "tx_cursor", "compass", "target", "scroll", "detect_", "menu"];
const UI_MAX_SIZE: usize = 256;
const UI_BIG: &[(&str, usize)] = &[("textures/menu_morrowind", 512)];
const ATLAS_KEY: &str = "ui-atlas/";
const ATLAS_WIDTH: usize = 512;
const ATLAS_HEIGHT: usize = 256;
const ATLAS_GUTTER: usize = 1;
const ATLAS_EXCLUDED: &[&str] = &["icons/", "bookart/", "textures/levelup/", "textures/birthsigns/"];
const ATLAS_MAX_SIDE: usize = 512;
const ATLAS_MAX_AREA: usize = 128 * 64;
const EXCLUDED: &[&str] = &["textures/omw_map_color_palette", "textures/omw/water_nm"];
const GLYPH_PREFIX: &str = "textures/omw_steam_button_";
const GLYPH_SIZE: usize = 32;
const PREFIXES: &[&str] = &["textures/", "icons/", "bookart/"];
const IMAGE_EXTENSIONS: &[&str] = &[".dds", ".tga", ".bmp", ".png", ".jpg", ".jpeg"];
const MAX_SIZE: usize = 256;

/// The texture names OpenMW's interface code uses (data/ui_code_textures.txt, refreshed by
/// tools/update_ui_code_textures.py): they go in the UI atlas.
const CODE_TEXTURES: &str = include_str!("../../../data/ui_code_textures.txt");

pub struct CookOptions {
    /// The game's Data Files (its archives and loose files), read only.
    pub data_files: PathBuf,
    /// In load order.
    pub archives: Vec<String>,
    /// OpenMW's own resources/vfs, read first (the port's files).
    pub engine_vfs: PathBuf,
    /// The port's 3DS interface (openmw/ui3ds), read last (the port's files).
    pub port_interface: PathBuf,
    /// The pack, textures.o3tp.
    pub out: PathBuf,
    /// 0: one per CPU core.
    pub threads: usize,
    /// Cook only the first N textures (testing).
    pub limit: usize,
    /// When set: each entry's hash and key, one per line (testing).
    pub key_list: Option<PathBuf>,
}

pub struct CookSummary {
    pub textures: usize,
    pub atlas_images: usize,
    pub bytes: u64,
}

fn starts_with_any(s: &str, prefixes: &[&str]) -> bool {
    prefixes.iter().any(|p| s.starts_with(p))
}

fn ends_with_any(s: &str, suffixes: &[&str]) -> bool {
    suffixes.iter().any(|x| s.ends_with(x))
}

fn normalize(path: &str) -> String {
    path.replace('\\', "/").to_ascii_lowercase()
}

/// os.path.splitext's root: the path without its last extension (a leading dot of the name isn't one).
fn stem_key(path: &str) -> &str {
    let name_start = path.rfind('/').map_or(0, |slash| slash + 1);
    let Some(dot) = path.rfind('.') else {
        return path;
    };
    if dot < name_start {
        return path;
    }
    let first_non_dot = path[name_start..].find(|c| c != '.').map_or(path.len(), |i| name_start + i);
    if dot < first_non_dot {
        return path;
    }
    &path[..dot]
}

fn fnv1a64(text: &str) -> u64 {
    let mut h = 0xCBF29CE484222325u64;
    for &c in text.as_bytes() {
        h ^= u64::from(c);
        h = h.wrapping_mul(0x100000001B3);
    }
    h
}

#[derive(Clone)]
struct Source {
    file: PathBuf,
    offset: u64,
    size: u64,
}

fn get32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

fn read_bsa_index(path: &Path) -> Result<BTreeMap<String, Source>> {
    let mut f = File::open(path).map_err(|e| io_error("can't read", path, e))?;
    let mut header = [0u8; 12];
    let truncated = || Error::Failed(format!("{} is truncated", path.display()));
    f.read_exact(&mut header).map_err(|_| truncated())?;
    let (version, hash_offset, count) = (get32(&header), get32(&header[4..]), get32(&header[8..]) as usize);
    if version != 0x100 {
        return fail(format!("{} is not a Morrowind archive", path.display()));
    }
    let mut records = vec![0u8; count * 8];
    let mut name_offsets = vec![0u8; count * 4];
    let mut names = vec![0u8; (hash_offset as usize).saturating_sub(12 * count)];
    f.read_exact(&mut records).map_err(|_| truncated())?;
    f.read_exact(&mut name_offsets).map_err(|_| truncated())?;
    f.read_exact(&mut names).map_err(|_| truncated())?;
    let data_start = 12 + u64::from(hash_offset) + 8 * count as u64;
    let mut files = BTreeMap::new();
    for i in 0..count {
        let (size, offset) = (get32(&records[i * 8..]), get32(&records[i * 8 + 4..]));
        let name_offset = get32(&name_offsets[i * 4..]) as usize;
        let Some(tail) = names.get(name_offset..) else {
            return Err(truncated());
        };
        let end = tail.iter().position(|&c| c == 0).unwrap_or(tail.len());
        let name = String::from_utf8_lossy(&tail[..end]);
        files.insert(
            normalize(&name),
            Source { file: path.to_path_buf(), offset: data_start + u64::from(offset), size: u64::from(size) },
        );
    }
    Ok(files)
}

/// A texture to cook: its file's name in the game's file system and where its bytes are.
struct Texture {
    name: String,
    source: Source,
}

/// key -> texture, the VFS order: OpenMW's resources, the game's archives, its loose files, the port's interface.
/// Then one texture per key, .dds preferred.
fn collect_sources(options: &CookOptions) -> Result<BTreeMap<String, Texture>> {
    let mut sources: BTreeMap<String, Source> = BTreeMap::new();
    let walk = |folder: &Path, sources: &mut BTreeMap<String, Source>| -> Result<()> {
        if folder.as_os_str().is_empty() || !folder.is_dir() {
            return Ok(());
        }
        let mut files = Vec::new();
        walk_files(folder, &mut files).map_err(|e| io_error("can't read", folder, e))?;
        for file in files {
            let relative = file.strip_prefix(folder).unwrap_or(&file);
            let parts: Vec<String> =
                relative.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect();
            let name = normalize(&parts.join("/"));
            if starts_with_any(&name, PREFIXES) && ends_with_any(&name, IMAGE_EXTENSIONS) {
                let size = file.metadata().map_err(|e| io_error("can't read", &file, e))?.len();
                sources.insert(name, Source { file, offset: 0, size });
            }
        }
        Ok(())
    };
    walk(&options.engine_vfs, &mut sources)?;
    for archive in &options.archives {
        for (name, source) in read_bsa_index(&options.data_files.join(archive))? {
            if starts_with_any(&name, PREFIXES) && ends_with_any(&name, IMAGE_EXTENSIONS) {
                sources.insert(name, source);
            }
        }
    }
    walk(&options.data_files, &mut sources)?;
    walk(&options.port_interface, &mut sources)?;

    let mut by_key: BTreeMap<String, Texture> = BTreeMap::new();
    for (name, source) in sources {
        let key = stem_key(&name).to_string();
        if starts_with_any(&key, EXCLUDED) {
            continue;
        }
        if !by_key.contains_key(&key) || name.ends_with(".dds") {
            by_key.insert(key, Texture { name, source });
        }
    }
    Ok(by_key)
}

/// The quoted values of texture="..." (the word in any case, spaces around the '='), as the Python cooker's
/// regular expression finds them.
fn texture_attributes(text: &[u8]) -> Vec<String> {
    let lower = text.to_ascii_lowercase();
    let is_space = |c: u8| matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0B | 0x0C);
    let mut found = Vec::new();
    let mut i = 0;
    while i + 7 <= lower.len() {
        if &lower[i..i + 7] != b"texture" {
            i += 1;
            continue;
        }
        let mut p = i + 7;
        while p < text.len() && is_space(text[p]) {
            p += 1;
        }
        if p < text.len() && text[p] == b'=' {
            p += 1;
            while p < text.len() && is_space(text[p]) {
                p += 1;
            }
            if p < text.len() && text[p] == b'"' {
                if let Some(length) = text[p + 1..].iter().position(|&c| c == b'"') {
                    if length > 0 {
                        found.push(String::from_utf8_lossy(&text[p + 1..p + 1 + length]).into_owned());
                        i = p + 1 + length + 1;
                        continue;
                    }
                }
            }
        }
        i += 1;
    }
    found
}

/// The textures the MyGUI skins and layouts reference (texture="..."), and the ones the interface code names
/// (embedded): only these go in the UI atlas.
fn referenced_textures(folders: &[PathBuf]) -> BTreeSet<String> {
    let mut names: BTreeSet<String> =
        CODE_TEXTURES.lines().map(str::trim_end).filter(|l| !l.is_empty()).map(String::from).collect();
    for folder in folders {
        let Ok(entries) = std::fs::read_dir(folder) else {
            continue;
        };
        for entry in entries.flatten() {
            let file = entry.file_name().to_string_lossy().into_owned();
            if !file.ends_with(".xml") && !file.ends_with(".layout") && !file.ends_with(".skin") {
                continue;
            }
            let Ok(text) = std::fs::read(entry.path()) else {
                continue;
            };
            for value in texture_attributes(&text) {
                names.insert(stem_key(&normalize(&value)).to_string());
            }
        }
    }
    names
}

fn is_ui(name: &str) -> bool {
    if starts_with_any(name, UI_PREFIXES) {
        return true;
    }
    if let Some(rest) = name.strip_prefix("textures/") {
        if !rest.contains('/') {
            return starts_with_any(rest, UI_TEXTURE_NAMES);
        }
    }
    false
}

fn is_atlas_candidate(key: &str, width: usize, height: usize) -> bool {
    !starts_with_any(key, ATLAS_EXCLUDED) && width.max(height) <= ATLAS_MAX_SIDE && width * height <= ATLAS_MAX_AREA
}

fn pow2_size(n: usize, max_size: usize) -> usize {
    let mut p = 8;
    while p < n && p < max_size {
        p *= 2;
    }
    if p > 8 && p > n && (p - n) as i64 > n as i64 - (p / 2) as i64 {
        p /= 2;
    }
    p.min(max_size).max(8)
}

fn pow2_ceil(n: usize, max_size: usize) -> usize {
    let mut p = 8;
    while p < n && p < max_size {
        p *= 2;
    }
    p
}

fn read_source(source: &Source) -> Result<Vec<u8>> {
    let read = || -> std::io::Result<Vec<u8>> {
        let mut f = File::open(&source.file)?;
        f.seek(SeekFrom::Start(source.offset))?;
        let mut data = vec![0u8; source.size as usize];
        f.read_exact(&mut data)?;
        Ok(data)
    };
    read().map_err(|e| io_error("can't read", &source.file, e))
}

struct Cooked {
    key: String,
    data: Vec<u8>,
    width: usize,
    height: usize,
    src_width: usize,
    src_height: usize,
    format: PicaFormat,
    levels: u8,
    flags: u8,
    atlas_texels: Option<Image>,
}

/// The images of one texture's steps, for debugging.
#[derive(Default)]
struct Steps {
    decoded: Image,
    /// What goes to the encoder (flipped).
    encoded: Image,
}

fn cook_one(key: &str, name: &str, raw: &[u8], referenced: bool, mut steps: Option<&mut Steps>) -> Result<Cooked> {
    let mut image = decode_image(raw, name)?;
    if let Some(steps) = steps.as_deref_mut() {
        steps.decoded = image.clone();
    }
    if key.starts_with(GLYPH_PREFIX) && image.width.max(image.height) > GLYPH_SIZE {
        image = resize(&image, GLYPH_SIZE, GLYPH_SIZE, Filter::Lanczos);
    }
    let (src_w, src_h) = (image.width, image.height);
    let ui = is_ui(key) || referenced;

    if name.ends_with(".tga") && tga_ignores_alpha(raw) {
        for p in image.pixels.chunks_exact_mut(4) {
            p[3] = 255;
        }
    }
    let mut rgba = image;

    let alphas = rgba.pixels.chunks_exact(4).map(|p| p[3]);
    let lo = alphas.clone().min().unwrap_or(255);
    let partial = alphas.clone().any(|a| a != 0 && a != 255);
    let has_alpha = lo < 255;
    let mut flags = 0u8;
    if has_alpha {
        flags |= FLAG_ALPHA;
        if !partial {
            flags |= FLAG_ALPHA_BINARY;
        }
    }

    let (w, h, format, resampled);
    if ui {
        let ui_max = UI_BIG.iter().find(|(k, _)| *k == key).map_or(UI_MAX_SIZE, |(_, size)| *size);
        resampled = src_w.max(src_h) > ui_max;
        w = pow2_ceil(src_w.min(ui_max), ui_max);
        h = pow2_ceil(src_h.min(ui_max), ui_max);
        format = if w.max(h) <= 128 {
            PicaFormat::Rgba8
        } else if has_alpha {
            PicaFormat::Etc1A4
        } else {
            PicaFormat::Etc1
        };
        if has_alpha {
            rgba = bleed_transparent(&rgba, format == PicaFormat::Rgba8);
        }
        if resampled {
            rgba = resize(&rgba, w, h, Filter::Lanczos);
        } else if w != src_w || h != src_h {
            rgba = padded_edge(&rgba, w, h);
            flags |= FLAG_PADDED;
        }
    } else {
        resampled = false;
        w = pow2_size(src_w, MAX_SIZE);
        h = pow2_size(src_h, MAX_SIZE);
        if w != src_w || h != src_h {
            let filter = if w < src_w || h < src_h { Filter::Lanczos } else { Filter::Bicubic };
            rgba = resize(&rgba, w, h, filter);
        }
        let small_raw = name.ends_with(".dds") && dds_is_uncompressed(raw) && w.max(h) <= 128;
        format = if small_raw {
            PicaFormat::Rgba8
        } else if has_alpha {
            PicaFormat::Etc1A4
        } else {
            PicaFormat::Etc1
        };
        if has_alpha {
            rgba = bleed_transparent(&rgba, format == PicaFormat::Rgba8);
        }
    }

    let mut atlas_texels = None;
    if ui && referenced && is_atlas_candidate(key, src_w, src_h) {
        let mut texels = if resampled { rgba.clone() } else { cropped(&rgba, src_w, src_h) };
        if key.starts_with(ATLAS_HALVED_PREFIX) && src_w.max(src_h) > 32 {
            texels = resize(&texels, src_w / 2, src_h / 2, Filter::Lanczos);
            flags |= FLAG_ATLAS_HALVED;
        }
        if texels.height > texels.width && texels.height > 64 {
            texels = transposed(&texels);
            flags |= FLAG_ATLAS_TRANSPOSED;
        }
        flags |= FLAG_ATLAS;
        atlas_texels = Some(texels);
    }

    let flipped = flip_vertical(&rgba);
    let (data, levels) = encode_pica(&flipped, format, !ui)?;
    if let Some(steps) = steps {
        steps.encoded = flipped;
    }
    Ok(Cooked {
        key: key.to_string(),
        data,
        width: w,
        height: h,
        src_width: src_w,
        src_height: src_h,
        format,
        levels,
        flags,
        atlas_texels,
    })
}

fn atlas_gutters(width: usize, height: usize) -> (usize, usize) {
    (
        if width >= ATLAS_WIDTH { 0 } else { ATLAS_GUTTER },
        if height >= ATLAS_HEIGHT { 0 } else { ATLAS_GUTTER },
    )
}

/// Guillotine packing of the one atlas page (pack_atlas): the long strips first, then the tallest, the widest. Each
/// cell takes the free rectangle with the smallest shorter leftover, turned on its side only when that is the only
/// way it fits. Returns each key's position and the turned keys.
fn pack_atlas(cells: &[(&str, &Image)]) -> Result<(BTreeMap<String, (usize, usize)>, BTreeSet<String>)> {
    #[derive(Clone, Copy)]
    struct Rect {
        x: usize,
        y: usize,
        w: usize,
        h: usize,
    }
    struct Sized {
        key: String,
        w: usize,
        h: usize,
        gx: usize,
        gy: usize,
    }
    let mut free = vec![Rect { x: 0, y: 0, w: ATLAS_WIDTH, h: ATLAS_HEIGHT }];
    let mut sized: Vec<Sized> = cells
        .iter()
        .map(|(key, texels)| {
            let (gx, gy) = atlas_gutters(texels.width, texels.height);
            Sized { key: key.to_string(), w: texels.width + 2 * gx, h: texels.height + 2 * gy, gx, gy }
        })
        .collect();
    sized.sort_by(|a, b| {
        let group = |s: &Sized| if s.w > ATLAS_WIDTH / 4 { 0 } else { 1 };
        group(a).cmp(&group(b)).then(b.h.cmp(&a.h)).then(b.w.cmp(&a.w)).then(a.key.cmp(&b.key))
    });
    let mut placements = BTreeMap::new();
    let mut turned = BTreeSet::new();
    for mut cell in sized {
        let mut best: Option<((usize, usize), usize, bool)> = None;
        for turn in [false, true] {
            if turn && cell.w == cell.h {
                break;
            }
            let (cw, ch) = if turn { (cell.h, cell.w) } else { (cell.w, cell.h) };
            for (i, f) in free.iter().enumerate() {
                if cw <= f.w && ch <= f.h {
                    let score = ((f.w - cw).min(f.h - ch), (f.w - cw).max(f.h - ch));
                    if best.is_none_or(|(best_score, _, _)| score < best_score) {
                        best = Some((score, i, turn));
                    }
                }
            }
            if best.is_some() {
                break;
            }
        }
        let Some((_, index, turn)) = best else {
            return fail(format!("the UI atlas is full at {}", cell.key));
        };
        if turn {
            turned.insert(cell.key.clone());
            std::mem::swap(&mut cell.w, &mut cell.h);
            std::mem::swap(&mut cell.gx, &mut cell.gy);
        }
        let f = free.remove(index);
        placements.insert(cell.key.clone(), (f.x + cell.gx, f.y + cell.gy));
        if f.w - cell.w > f.h - cell.h {
            free.push(Rect { x: f.x + cell.w, y: f.y, w: f.w - cell.w, h: f.h });
            free.push(Rect { x: f.x, y: f.y + cell.h, w: cell.w, h: f.h - cell.h });
        } else {
            free.push(Rect { x: f.x, y: f.y + cell.h, w: f.w, h: f.h - cell.h });
            free.push(Rect { x: f.x + cell.w, y: f.y, w: f.w - cell.w, h: cell.h });
        }
        free.retain(|r| r.w > 0 && r.h > 0);
    }
    Ok((placements, turned))
}

struct Entry {
    hash: u64,
    key: String,
    data: Vec<u8>,
    width: usize,
    height: usize,
    src_width: usize,
    src_height: usize,
    format: PicaFormat,
    levels: u8,
    flags: u8,
    atlas: u8,
    atlas_pos: u32,
}

fn put(out: &mut Vec<u8>, value: u64, bytes: usize) {
    for i in 0..bytes {
        out.push((value >> (8 * i)) as u8);
    }
}

fn align(value: usize) -> usize {
    value.div_ceil(DATA_ALIGN) * DATA_ALIGN
}

/// What the cooking reads, as one line per texture and file: an install cooked from the same lines (and the same
/// revision of the cooker) has the same pack. Sizes and modification times, not contents (reading them all would
/// take as long as cooking).
pub fn source_fingerprint(options: &CookOptions) -> Result<String> {
    let sources = collect_sources(options)?;
    let referenced = referenced_textures(&[options.engine_vfs.join("mygui"), options.port_interface.join("mygui")]);
    let mut text = format!("cooker {VERSION}.{REVISION}\n");
    let mut stamps: BTreeMap<&Path, String> = BTreeMap::new();
    for texture in sources.values() {
        stamps.entry(&texture.source.file).or_insert_with(|| file_stamp(&texture.source.file));
    }
    for (key, texture) in &sources {
        let s = &texture.source;
        text += &format!("{key} {} {} {} {}\n", texture.name, stamps[s.file.as_path()], s.offset, s.size);
    }
    for name in referenced {
        text += &format!("ref {name}\n");
    }
    Ok(format!("{:016x}", fnv1a64(&text)))
}

/// How many textures the pack would hold (the atlas included).
pub fn texture_count(options: &CookOptions) -> Result<usize> {
    Ok(collect_sources(options)?.len() + 1)
}

/// A file's size and modification time.
pub(crate) fn file_stamp(path: &Path) -> String {
    let Ok(metadata) = path.metadata() else {
        return "missing".to_string();
    };
    let modified = metadata.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_nanos());
    format!("{}@{modified}", metadata.len())
}

/// Cooks the textures for the 3DS's GPU into one pack. `progress` gets the textures done and the total (from the
/// cooking threads). `cancel`: set by another thread, the cooking stops and nothing is written.
pub fn cook_textures(
    options: &CookOptions,
    cancel: Option<&AtomicBool>,
    progress: &(dyn Fn(usize, usize) + Sync),
) -> Result<CookSummary> {
    etc1::init();
    let cancelled = || cancel.is_some_and(|c| c.load(Ordering::Relaxed));
    let sources = collect_sources(options)?;
    let mut referenced =
        referenced_textures(&[options.engine_vfs.join("mygui"), options.port_interface.join("mygui")]);
    for key in sources.keys() {
        if key.starts_with(ATLAS_HALVED_PREFIX) {
            referenced.insert(key.clone());
        }
    }

    let mut keys: Vec<&String> = sources.keys().collect();
    if options.limit != 0 && keys.len() > options.limit {
        keys.truncate(options.limit);
    }

    let results: Vec<Mutex<Option<Cooked>>> = keys.iter().map(|_| Mutex::new(None)).collect();
    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let error: Mutex<Option<String>> = Mutex::new(None);
    let progress_lock = Mutex::new(());
    let threads = if options.threads > 0 {
        options.threads
    } else {
        std::thread::available_parallelism().map_or(1, usize::from)
    };
    std::thread::scope(|scope| {
        for _ in 0..threads {
            scope.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    if i >= keys.len() || cancelled() || error.lock().unwrap().is_some() {
                        return;
                    }
                    let texture = &sources[keys[i]];
                    let cooked = read_source(&texture.source).and_then(|raw| {
                        cook_one(keys[i], &texture.name, &raw, referenced.contains(keys[i]), None)
                    });
                    match cooked {
                        Ok(cooked) => *results[i].lock().unwrap() = Some(cooked),
                        Err(e) => {
                            error.lock().unwrap().get_or_insert(format!("{}: {e}", texture.name));
                            return;
                        }
                    }
                    let finished = done.fetch_add(1, Ordering::Relaxed) + 1;
                    let _lock = progress_lock.lock().unwrap();
                    progress(finished, keys.len());
                }
            });
        }
    });
    if cancelled() {
        return Err(Error::Cancelled);
    }
    if let Some(error) = error.into_inner().unwrap() {
        return fail(format!("texture {error}"));
    }
    let results: Vec<Cooked> = results.into_iter().map(|r| r.into_inner().unwrap().unwrap()).collect();

    // The UI atlas.
    let cells: Vec<(&str, &Image)> =
        results.iter().filter_map(|r| r.atlas_texels.as_ref().map(|t| (r.key.as_str(), t))).collect();
    let (placements, turned) = pack_atlas(&cells)?;
    let mut canvas = Image::new(ATLAS_WIDTH, ATLAS_HEIGHT);
    for (key, texels) in &cells {
        let texels = if turned.contains(*key) { transposed(texels) } else { (*texels).clone() };
        let (gx, gy) = atlas_gutters(texels.width, texels.height);
        let padded = padded_edge_around(&texels, gx, gy, gx, gy);
        let (px, py) = placements[*key];
        for y in 0..padded.height {
            let o = canvas.offset(px - gx, py - gy + y);
            canvas.pixels[o..o + padded.width * 4].copy_from_slice(padded.row(y));
        }
    }
    let atlas_images = cells.len();

    let mut entries: Vec<Entry> = Vec::with_capacity(results.len() + 1);
    for r in results {
        let (px, py) = placements.get(&r.key).copied().unwrap_or((0, 0));
        let mut flags = r.flags;
        if turned.contains(&r.key) {
            flags |= FLAG_ATLAS_TRANSPOSED;
        }
        entries.push(Entry {
            hash: fnv1a64(&r.key),
            key: r.key,
            data: r.data,
            width: r.width,
            height: r.height,
            src_width: r.src_width,
            src_height: r.src_height,
            format: r.format,
            levels: r.levels,
            flags,
            atlas: 0,
            atlas_pos: px as u32 | ((py as u32) << 16),
        });
    }
    let key = format!("{ATLAS_KEY}0");
    let (data, _) = encode_pica(&flip_vertical(&canvas), PicaFormat::Rgba8, false)?;
    entries.push(Entry {
        hash: fnv1a64(&key),
        key,
        data,
        width: ATLAS_WIDTH,
        height: ATLAS_HEIGHT,
        src_width: ATLAS_WIDTH,
        src_height: ATLAS_HEIGHT,
        format: PicaFormat::Rgba8,
        levels: 1,
        flags: FLAG_ALPHA,
        atlas: 0,
        atlas_pos: 0,
    });
    entries.sort_by_key(|e| e.hash);
    for pair in entries.windows(2) {
        if pair[0].hash == pair[1].hash {
            return fail(format!("texture name hash collision: {} / {}", pair[0].key, pair[1].key));
        }
    }

    if let Some(list) = &options.key_list {
        let text: String = entries.iter().map(|e| format!("{:016x} {}\n", e.hash, e.key)).collect();
        std::fs::write(list, text).map_err(|e| io_error("can't write", list, e))?;
    }

    // The pack: header, index, then each texture 128-byte aligned.
    let index_offset = HEADER_SIZE;
    let data_offset = align(index_offset + ENTRY_SIZE * entries.len());
    let mut head = b"O3TP".to_vec();
    put(&mut head, u64::from(VERSION), 4);
    put(&mut head, entries.len() as u64, 4);
    put(&mut head, index_offset as u64, 4);
    put(&mut head, data_offset as u64, 4);
    put(&mut head, 0, 12);
    let mut offset = data_offset;
    for e in &entries {
        put(&mut head, e.hash, 8);
        put(&mut head, offset as u64, 4);
        put(&mut head, e.data.len() as u64, 4);
        put(&mut head, e.width as u64, 2);
        put(&mut head, e.height as u64, 2);
        put(&mut head, e.src_width as u64, 2);
        put(&mut head, e.src_height as u64, 2);
        put(&mut head, e.format as u64, 1);
        put(&mut head, u64::from(e.levels), 1);
        put(&mut head, u64::from(e.flags), 1);
        put(&mut head, u64::from(e.atlas), 1);
        put(&mut head, u64::from(e.atlas_pos), 4);
        offset = align(offset + e.data.len());
    }
    head.resize(data_offset, 0);

    if let Some(parent) = options.out.parent() {
        std::fs::create_dir_all(parent).map_err(|e| io_error("can't create", parent, e))?;
    }
    let part = PathBuf::from(format!("{}.part", options.out.display()));
    let write_error = |e| io_error("can't write", &part, e);
    let mut size = head.len();
    {
        let mut file = BufWriter::with_capacity(1 << 20, File::create(&part).map_err(write_error)?);
        file.write_all(&head).map_err(write_error)?;
        let zeros = [0u8; DATA_ALIGN];
        for e in &entries {
            let aligned = align(size);
            file.write_all(&zeros[..aligned - size]).map_err(write_error)?;
            file.write_all(&e.data).map_err(write_error)?;
            size = aligned + e.data.len();
        }
        file.flush().map_err(write_error)?;
    }
    std::fs::rename(&part, &options.out).map_err(|e| io_error("can't write", &options.out, e))?;
    Ok(CookSummary { textures: entries.len(), atlas_images, bytes: size as u64 })
}

/// Debug: the steps of one texture (its decoded image, the image handed to the encoder) as raw RGBA files in
/// `folder`, named <step>_<width>x<height>.rgba.
pub fn dump_texture_steps(options: &CookOptions, key: &str, folder: &Path) -> Result<()> {
    etc1::init();
    let sources = collect_sources(options)?;
    let Some(texture) = sources.get(key) else {
        return fail(format!("no texture {key}"));
    };
    let referenced = referenced_textures(&[options.engine_vfs.join("mygui"), options.port_interface.join("mygui")]);
    let mut steps = Steps::default();
    let raw = read_source(&texture.source)?;
    cook_one(
        key,
        &texture.name,
        &raw,
        referenced.contains(key) || key.starts_with(ATLAS_HALVED_PREFIX),
        Some(&mut steps),
    )?;
    std::fs::create_dir_all(folder).map_err(|e| io_error("can't create", folder, e))?;
    for (step, image) in [("decoded", &steps.decoded), ("encoded", &steps.encoded)] {
        let path = folder.join(format!("{step}_{}x{}.rgba", image.width, image.height));
        std::fs::write(&path, &image.pixels).map_err(|e| io_error("can't write", &path, e))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stems() {
        assert_eq!(stem_key("textures/a.b.dds"), "textures/a.b");
        assert_eq!(stem_key("textures/.hidden"), "textures/.hidden");
        assert_eq!(stem_key("tex.tures/name"), "tex.tures/name");
    }

    #[test]
    fn finds_texture_attributes() {
        let found = texture_attributes(b"<Skin Texture = \"a.dds\"/><x texture=\"\"/> texture=\"b\"");
        assert_eq!(found, vec!["a.dds", "b"]);
    }

    #[test]
    fn sizes() {
        assert_eq!(pow2_size(100, 256), 128);
        assert_eq!(pow2_size(90, 256), 64);
        assert_eq!(pow2_size(512, 256), 256);
        assert_eq!(pow2_ceil(100, 256), 128);
    }
}
