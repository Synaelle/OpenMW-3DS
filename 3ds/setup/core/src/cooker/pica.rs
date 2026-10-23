//! Textures as the 3DS's GPU reads them, with tex3ds's arithmetic (devkitPro's texture tool, the cooker's
//! reference).

use std::sync::OnceLock;

use super::etc1;
use super::image::Image;
use crate::{fail, Result};

/// GPU_TEXCOLOR values (libctru).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PicaFormat {
    Rgba8 = 0x0,
    Etc1 = 0xC,
    Etc1A4 = 0xD,
}

pub fn bits_per_pixel(format: PicaFormat) -> usize {
    match format {
        PicaFormat::Rgba8 => 32,
        PicaFormat::Etc1 => 4,
        PicaFormat::Etc1A4 => 8,
    }
}

/// A level as tex3ds holds it: ImageMagick quanta (0..65535, not rounded: an HDRI build) per channel.
struct Level {
    width: usize,
    height: usize,
    rgba: Vec<f64>,
}

impl Level {
    fn at(&self, x: usize, y: usize) -> &[f64] {
        let o = (y * self.width + x) * 4;
        &self.rgba[o..o + 4]
    }
}

const QUANTUM_RANGE: f64 = 65535.0;

/// quantum_to_bits<bits> (tex3ds quantum.h): (1 << bits) * v / (QuantumRange + 1), truncated.
fn to_bits(bits: u32, v: f64) -> u8 {
    let r = f64::from(1u32 << bits) * v / (QUANTUM_RANGE + 1.0);
    if r <= 0.0 { 0 } else { r as u8 }
}

/// load_image: the image over a transparent one. Alpha 0 leaves black, otherwise the colour is unchanged.
fn base_level(image: &Image) -> Level {
    let mut rgba = vec![0f64; image.pixels.len()];
    for (out, p) in rgba.chunks_exact_mut(4).zip(image.pixels.chunks_exact(4)) {
        let a = p[3];
        for c in 0..3 {
            out[c] = if a == 0 { 0.0 } else { f64::from(p[c]) * 257.0 };
        }
        out[3] = f64::from(a) * 257.0;
    }
    Level { width: image.width, height: image.height, rgba }
}

/// ImageMagick's resize with the box filter from the base level to width x height (a whole power of two smaller):
/// each texel the average of its block, the colours weighted by alpha.
fn box_level(base: &Level, width: usize, height: usize) -> Level {
    let mut rgba = vec![0f64; width * height * 4];
    let (fx, fy) = (base.width / width, base.height / height);
    let count = fx as f64 * fy as f64;
    for y in 0..height {
        for x in 0..width {
            let (mut alpha_weight, mut color, mut alpha) = (0f64, [0f64; 3], 0f64);
            for by in 0..fy {
                for bx in 0..fx {
                    let p = base.at(x * fx + bx, y * fy + by);
                    let a = p[3] / QUANTUM_RANGE;
                    alpha_weight += a;
                    for c in 0..3 {
                        color[c] += a * p[c];
                    }
                    alpha += p[3];
                }
            }
            let out = &mut rgba[(y * width + x) * 4..(y * width + x) * 4 + 4];
            for c in 0..3 {
                out[c] = if alpha_weight > 0.0 { color[c] / alpha_weight } else { 0.0 };
            }
            out[3] = alpha / count;
        }
    }
    Level { width, height, rgba }
}

/// ETC1 at medium quality in 8x8 tiles of four 4x4 blocks (ETC1A4: each block's 4-bit alpha first).
fn encode_etc1(level: &Level, alpha: bool, out: &mut Vec<u8>) {
    for ty in (0..level.height).step_by(8) {
        for tx in (0..level.width).step_by(8) {
            for j in [0, 4] {
                for i in [0, 4] {
                    let mut block = [[0u8; 4]; 16];
                    let mut alpha_bits = [0u8; 8];
                    for y in 0..4 {
                        for x in 0..4 {
                            let p = level.at(tx + i + x, ty + j + y);
                            block[y * 4 + x] = [to_bits(8, p[0]), to_bits(8, p[1]), to_bits(8, p[2]), 0xFF];
                            // 4-bit alpha, the X and Y axes swapped.
                            alpha_bits[2 * x + y / 2] |= to_bits(4, p[3]) << if y & 1 != 0 { 4 } else { 0 };
                        }
                    }
                    let packed = etc1::pack_block(&block);
                    if alpha {
                        out.extend_from_slice(&alpha_bits);
                    }
                    // rg_etc1 writes big-endian, the GPU reads little-endian.
                    out.extend(packed.iter().rev());
                }
            }
        }
    }
}

/// tex3ds swizzle.cpp: the 8x8 tile's texels moved into Morton order by rotating these foursomes, then swapping
/// four pairs. Output slot i holds the tile texel order[i] (y * 8 + x).
fn morton_order() -> &'static [usize; 64] {
    static ORDER: OnceLock<[usize; 64]> = OnceLock::new();
    ORDER.get_or_init(|| {
        const TABLE: [[usize; 4]; 12] = [
            [2, 8, 16, 4],
            [3, 9, 17, 5],
            [6, 10, 24, 20],
            [7, 11, 25, 21],
            [14, 26, 28, 22],
            [15, 27, 29, 23],
            [34, 40, 48, 36],
            [35, 41, 49, 37],
            [38, 42, 56, 52],
            [39, 43, 57, 53],
            [46, 58, 60, 54],
            [47, 59, 61, 55],
        ];
        let mut p: [usize; 64] = std::array::from_fn(|i| i);
        for e in TABLE {
            let tmp = p[e[0]];
            p[e[0]] = p[e[1]];
            p[e[1]] = p[e[2]];
            p[e[2]] = p[e[3]];
            p[e[3]] = tmp;
        }
        p.swap(12, 18);
        p.swap(13, 19);
        p.swap(44, 50);
        p.swap(45, 51);
        p
    })
}

/// RGBA8 in Morton order, A B G R.
fn encode_rgba8(level: &Level, out: &mut Vec<u8>) {
    let order = morton_order();
    for ty in (0..level.height).step_by(8) {
        for tx in (0..level.width).step_by(8) {
            for &t in order {
                let p = level.at(tx + t % 8, ty + t / 8);
                out.extend_from_slice(&[to_bits(8, p[3]), to_bits(8, p[2]), to_bits(8, p[1]), to_bits(8, p[0])]);
            }
        }
    }
}

/// Encodes an image (sides multiples of 8, bottom row first: the cooker flips it) as the 3DS GPU reads it: every
/// pixel composited over transparent black first (alpha 0 texels become black), mip levels down to a side of 8 when
/// `mipmaps` (each one the alpha-weighted average of the base level's blocks, as ImageMagick's box filter gives it).
/// Returns the levels back to back and their count.
pub fn encode_pica(image: &Image, format: PicaFormat, mipmaps: bool) -> Result<(Vec<u8>, u8)> {
    if image.width < 8 || image.height < 8 || image.width % 8 != 0 || image.height % 8 != 0 {
        return fail("texture sides must be multiples of 8");
    }
    let base = base_level(image);
    let mut out = Vec::with_capacity(image.width * image.height * bits_per_pixel(format) / 8 * 4 / 3 + 64);
    let mut levels = 0u8;
    let (mut w, mut h) = (image.width, image.height);
    loop {
        let level = if levels == 0 { None } else { Some(box_level(&base, w, h)) };
        let current = level.as_ref().unwrap_or(&base);
        match format {
            PicaFormat::Rgba8 => encode_rgba8(current, &mut out),
            _ => encode_etc1(current, format == PicaFormat::Etc1A4, &mut out),
        }
        levels += 1;
        if !mipmaps || w <= 8 || h <= 8 {
            break;
        }
        w /= 2;
        h /= 2;
    }
    Ok((out, levels))
}
