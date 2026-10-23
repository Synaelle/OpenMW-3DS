//! An ETC1 block packer: rg_etc1 1.04 by Rich Geldreich (rg_etc1.cpp, from devkitPro's tex3ds, the cooker's
//! reference), ported to Rust for the setup app. Altered from the original: only what the cooker uses is kept
//! (medium quality, no dithering, no unpacking), the arithmetic is unchanged so the blocks are the same bytes.
//!
//! rg_etc1 uses the zlib licence:
//!
//! Copyright (c) 2012 Rich Geldreich
//!
//! This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
//! liable for any damages arising from the use of this software.
//!
//! Permission is granted to anyone to use this software for any purpose, including commercial applications, and to
//! alter it and redistribute it freely, subject to the following restrictions:
//!
//! 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original
//!    software. If you use this software in a product, an acknowledgment in the product documentation would be
//!    appreciated but is not required.
//! 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
//!    software.
//! 3. This notice may not be removed or altered from any source distribution.

use std::sync::OnceLock;

use super::etc1_tables::{CONFIG_0_255, CONFIG_1_TO_254};

const INTEN_TABLES: [[i32; 4]; 8] = [
    [-8, -2, 2, 8],
    [-17, -5, 5, 17],
    [-29, -9, 9, 29],
    [-42, -13, 13, 42],
    [-60, -18, 18, 60],
    [-80, -24, 24, 80],
    [-106, -33, 33, 106],
    [-183, -47, 47, 183],
];

const SELECTOR_INDEX_TO_ETC1: [u32; 4] = [3, 2, 0, 1];
const DELTA_MIN: i32 = -4;
const DELTA_MAX: i32 = 3;
const NEXT_COMP: [usize; 4] = [1, 2, 0, 1];

#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct Color {
    c: [u8; 4],
}

impl Color {
    /// Clamped to 0..255 (color_quad_u8::set).
    fn set(r: i32, g: i32, b: i32, a: i32) -> Self {
        let clamp = |v: i32| v.clamp(0, 255) as u8;
        Color { c: [clamp(r), clamp(g), clamp(b), clamp(a)] }
    }

    fn r(&self) -> i32 {
        i32::from(self.c[0])
    }

    fn g(&self) -> i32 {
        i32::from(self.c[1])
    }

    fn b(&self) -> i32 {
        i32::from(self.c[2])
    }

    fn squared_distance_rgb(&self, other: &Color) -> u32 {
        let d = |i: usize| (i32::from(self.c[i]) - i32::from(other.c[i])).pow(2) as u32;
        d(0) + d(1) + d(2)
    }
}

/// [diff/inten_table/selector][desired colour]: the best packed colour in the low byte, the abs error in the high
/// byte.
fn inverse_lookup() -> &'static [[u16; 256]; 64] {
    static TABLE: OnceLock<Box<[[u16; 256]; 64]>> = OnceLock::new();
    TABLE.get_or_init(|| {
        let mut table = Box::new([[0u16; 256]; 64]);
        for diff in 0..2u32 {
            let limit = if diff != 0 { 32 } else { 16 };
            for inten in 0..8u32 {
                for selector in 0..4u32 {
                    let index = (diff + (inten << 1) + (selector << 4)) as usize;
                    for color in 0..256u32 {
                        let (mut best_error, mut best_packed) = (u32::MAX, 0);
                        for packed in 0..limit {
                            let v = decode_value(diff, inten, selector, packed);
                            let error = v.abs_diff(color);
                            if error < best_error {
                                best_error = error;
                                best_packed = packed;
                                if best_error == 0 {
                                    break;
                                }
                            }
                        }
                        table[index][color as usize] = (best_packed | (best_error << 8)) as u16;
                    }
                }
            }
        }
        table
    })
}

fn decode_value(diff: u32, inten: u32, selector: u32, packed: u32) -> u32 {
    let c = if diff != 0 { (packed >> 2) | (packed << 3) } else { packed | (packed << 4) } as i32;
    (c + INTEN_TABLES[inten as usize][selector as usize]).clamp(0, 255) as u32
}

/// The configurations giving an 8-bit value exactly, up to the end mark.
fn configs(value: i32) -> impl Iterator<Item = u32> {
    let table: &'static [u16] = match value {
        0 => &CONFIG_0_255[0],
        255 => &CONFIG_0_255[1],
        _ => &CONFIG_1_TO_254[(value - 1) as usize],
    };
    table.iter().take_while(|&&x| x != 0xFFFF).map(|&x| u32::from(x))
}

/// Builds the encoder's tables (pack_etc1_block_init). Packing calls it too: calling it first only moves the time.
pub fn init() {
    inverse_lookup();
}

#[derive(Clone, Copy, Default)]
struct Coords {
    unscaled: Color,
    inten_table: u32,
    color4: bool,
}

impl Coords {
    fn new(r: i32, g: i32, b: i32, color4: bool) -> Self {
        Coords { unscaled: Color::set(r, g, b, 255), inten_table: 0, color4 }
    }

    fn scaled(&self) -> Color {
        let u = &self.unscaled;
        let scale = |v: i32| if self.color4 { v | (v << 4) } else { (v >> 2) | (v << 3) };
        Color::set(scale(u.r()), scale(u.g()), scale(u.b()), 255)
    }
}

#[derive(Clone, Copy)]
struct Solution {
    coords: Coords,
    selectors: [u8; 8],
    error: u64,
    valid: bool,
}

impl Default for Solution {
    fn default() -> Self {
        Solution { coords: Coords::default(), selectors: [0; 8], error: u64::MAX, valid: false }
    }
}

#[derive(Clone, Copy, Default)]
struct Results {
    error: u64,
    color_unscaled: Color,
    inten_table: u32,
    selectors: [u8; 8],
    color4: bool,
}

struct Params<'a> {
    pixels: &'a [Color; 8],
    use_color4: bool,
    scan_deltas: &'static [i32],
    base_color5: Color,
    constrain_against_base_color5: bool,
}

/// etc1_optimizer at medium quality: the fast evaluation over a presorted luma.
struct Optimizer {
    limit: i32,
    avg_color: [f32; 3],
    br: i32,
    bg: i32,
    bb: i32,
    sorted_luma_indices: [usize; 8],
    sorted_luma: [u32; 8],
    best: Solution,
    trial: Solution,
    temp_selectors: [u8; 8],
}

impl Optimizer {
    fn new(params: &Params) -> Self {
        let limit = if params.use_color4 { 15 } else { 31 };
        let mut sum = [0f32; 3];
        let mut luma = [0u16; 8];
        for (i, c) in params.pixels.iter().enumerate() {
            for (s, &v) in sum.iter_mut().zip(&c.c[..3]) {
                *s += f32::from(v);
            }
            luma[i] = u16::from(c.c[0]) + u16::from(c.c[1]) + u16::from(c.c[2]);
        }
        let avg_color = sum.map(|s| s * (1.0 / 8.0));
        let quantize = |v: f32| ((v * limit as f32 / 255.0 + 0.5) as u32 as i32).clamp(0, limit);
        // A stable sort by luma (rg_etc1's indirect radix sort).
        let mut sorted_luma_indices = [0usize, 1, 2, 3, 4, 5, 6, 7];
        sorted_luma_indices.sort_by_key(|&i| luma[i]);
        Optimizer {
            limit,
            avg_color,
            br: quantize(avg_color[0]),
            bg: quantize(avg_color[1]),
            bb: quantize(avg_color[2]),
            sorted_luma_indices,
            sorted_luma: sorted_luma_indices.map(|i| u32::from(luma[i])),
            best: Solution::default(),
            trial: Solution::default(),
            temp_selectors: [0; 8],
        }
    }

    /// evaluate_solution_fast: the best intensity table and selectors for these coordinates, kept when better.
    fn evaluate(&mut self, params: &Params, coords: &Coords) -> bool {
        if params.constrain_against_base_color5 {
            let base = &params.base_color5;
            let d = [coords.unscaled.r() - base.r(), coords.unscaled.g() - base.g(), coords.unscaled.b() - base.b()];
            if d.iter().min().copied().unwrap_or(0) < DELTA_MIN || d.iter().max().copied().unwrap_or(0) > DELTA_MAX {
                self.trial.valid = false;
                return false;
            }
        }
        let base = coords.scaled();
        let pixels = params.pixels;
        self.trial.error = u64::MAX;
        for inten_table in (0..8u32).rev() {
            let table = &INTEN_TABLES[inten_table as usize];
            let mut block_colors = [Color::default(); 4];
            let mut block_inten = [0u32; 4];
            for s in 0..4 {
                let yd = table[s];
                block_colors[s] = Color::set(base.r() + yd, base.g() + yd, base.b() + yd, 0);
                block_inten[s] = (block_colors[s].r() + block_colors[s].g() + block_colors[s].b()) as u32;
            }
            // The inputs are presorted along the intensity axis, as the block colours are: 0 01 1 12 2 23 3.
            let midpoints = [block_inten[0] + block_inten[1], block_inten[1] + block_inten[2], block_inten[2] + block_inten[3]];
            let sorted = &self.sorted_luma;
            let mut total_error = 0u64;
            if sorted[7] * 2 < midpoints[0] {
                if block_inten[0] > sorted[7] && u64::from(block_inten[0].abs_diff(sorted[7])) >= self.trial.error {
                    continue;
                }
                self.temp_selectors = [0; 8];
                for p in pixels {
                    total_error += u64::from(block_colors[0].squared_distance_rgb(p));
                }
            } else if sorted[0] * 2 >= midpoints[2] {
                if sorted[0] > block_inten[3] && u64::from(sorted[0].abs_diff(block_inten[3])) >= self.trial.error {
                    continue;
                }
                self.temp_selectors = [3; 8];
                for p in pixels {
                    total_error += u64::from(block_colors[3].squared_distance_rgb(p));
                }
            } else {
                let mut selector = 0usize;
                let mut c = 0;
                'classify: while c < 8 {
                    let y = sorted[c];
                    while y * 2 >= midpoints[selector] {
                        selector += 1;
                        if selector > 2 {
                            break 'classify;
                        }
                    }
                    let index = self.sorted_luma_indices[c];
                    self.temp_selectors[index] = selector as u8;
                    total_error += u64::from(block_colors[selector].squared_distance_rgb(&pixels[index]));
                    c += 1;
                }
                while c < 8 {
                    let index = self.sorted_luma_indices[c];
                    self.temp_selectors[index] = 3;
                    total_error += u64::from(block_colors[3].squared_distance_rgb(&pixels[index]));
                    c += 1;
                }
            }
            if total_error < self.trial.error {
                self.trial.error = total_error;
                self.trial.coords.inten_table = inten_table;
                self.trial.selectors = self.temp_selectors;
                self.trial.valid = true;
                if total_error == 0 {
                    break;
                }
            }
        }
        self.trial.coords.unscaled = coords.unscaled;
        self.trial.coords.color4 = params.use_color4;
        if self.trial.error < self.best.error {
            self.best = self.trial;
            return true;
        }
        false
    }

    /// Scans the lattice around the average colour, refining each better solution: the best one, or None.
    fn compute(&mut self, params: &Params) -> Option<Results> {
        let deltas = params.scan_deltas;
        for &zd in deltas {
            let mbb = self.bb + zd;
            if mbb < 0 {
                continue;
            } else if mbb > self.limit {
                break;
            }
            for &yd in deltas {
                let mbg = self.bg + yd;
                if mbg < 0 {
                    continue;
                } else if mbg > self.limit {
                    break;
                }
                for &xd in deltas {
                    let mbr = self.br + xd;
                    if mbr < 0 {
                        continue;
                    } else if mbr > self.limit {
                        break;
                    }
                    let coords = Coords::new(mbr, mbg, mbb, params.use_color4);
                    if !self.evaluate(params, &coords) {
                        continue;
                    }
                    // The best block colour for these selectors: the average colour minus the average intensity
                    // delta (clamped), quantised.
                    let trials = if (xd | yd | zd) == 0 { 4 } else { 2 };
                    for _ in 0..trials {
                        let table = &INTEN_TABLES[self.best.coords.inten_table as usize];
                        let base = self.best.coords.scaled();
                        let (mut dr, mut dg, mut db) = (0i32, 0i32, 0i32);
                        for &s in &self.best.selectors {
                            let yd = table[usize::from(s)];
                            dr += (base.r() + yd).clamp(0, 255) - base.r();
                            dg += (base.g() + yd).clamp(0, 255) - base.g();
                            db += (base.b() + yd).clamp(0, 255) - base.b();
                        }
                        if dr == 0 && dg == 0 && db == 0 {
                            break;
                        }
                        let limit = self.limit;
                        let refine = |avg: f32, delta: i32| {
                            let avg_delta = delta as f32 / 8.0;
                            (((avg - avg_delta) * limit as f32 / 255.0 + 0.5) as u32 as i32).clamp(0, limit)
                        };
                        let br1 = refine(self.avg_color[0], dr);
                        let bg1 = refine(self.avg_color[1], dg);
                        let bb1 = refine(self.avg_color[2], db);
                        let best = &self.best.coords.unscaled;
                        if (mbr == br1 && mbg == bg1 && mbb == bb1)
                            || (br1 == best.r() && bg1 == best.g() && bb1 == best.b())
                            || (self.br == br1 && self.bg == bg1 && self.bb == bb1)
                        {
                            break;
                        }
                        if !self.evaluate(params, &Coords::new(br1, bg1, bb1, params.use_color4)) {
                            break;
                        }
                    }
                }
            }
        }
        if !self.best.valid {
            return None;
        }
        Some(Results {
            error: self.best.error,
            color_unscaled: self.best.coords.unscaled,
            inten_table: self.best.coords.inten_table,
            selectors: self.best.selectors,
            color4: self.best.coords.color4,
        })
    }
}

/// A solid block: the precomputed lists of exact configurations for each 8-bit value.
fn pack_solid(block: &mut [u8; 8], color: &Color) -> u64 {
    let inverse = inverse_lookup();
    let (mut best_error, mut best_i) = (u32::MAX, 0usize);
    let (mut best_x, mut best_c1, mut best_c2) = (0u32, 0u32, 0u32);
    'search: for i in 0..3 {
        let (c1, c2) = (usize::from(color.c[NEXT_COMP[i]]), usize::from(color.c[NEXT_COMP[i + 1]]));
        for delta in -1..=1 {
            let c_plus_delta = (i32::from(color.c[i]) + delta).clamp(0, 255);
            for x in configs(c_plus_delta) {
                let table = &inverse[(x & 0xFF) as usize];
                let (p1, p2) = (u32::from(table[c1]), u32::from(table[c2]));
                let trial = (c_plus_delta - i32::from(color.c[i])).pow(2) as u32 + (p1 >> 8).pow(2) + (p2 >> 8).pow(2);
                if trial < best_error {
                    best_error = trial;
                    best_x = x;
                    best_c1 = p1 & 0xFF;
                    best_c2 = p2 & 0xFF;
                    best_i = i;
                    if best_error == 0 {
                        break 'search;
                    }
                }
            }
        }
    }
    let diff = best_x & 1;
    let inten = (best_x >> 1) & 7;
    block[3] = (((inten | (inten << 3)) << 2) | (diff << 1)) as u8;
    let selector = SELECTOR_INDEX_TO_ETC1[((best_x >> 4) & 3) as usize];
    block[4] = if selector & 2 != 0 { 0xFF } else { 0 };
    block[5] = block[4];
    block[6] = if selector & 1 != 0 { 0xFF } else { 0 };
    block[7] = block[6];
    let c0 = (best_x >> 8) & 255;
    let pack = |c: u32| if diff != 0 { (c << 3) as u8 } else { (c | (c << 4)) as u8 };
    block[best_i] = pack(c0);
    block[NEXT_COMP[best_i]] = pack(best_c1);
    block[NEXT_COMP[best_i + 1]] = pack(best_c2);
    u64::from(best_error)
}

/// A solid subblock, with or without the differential mode, against the first subblock's base colour.
fn pack_solid_constrained(results: &mut Results, color: &Color, use_diff: bool, base: Option<&Color>) {
    let inverse = inverse_lookup();
    let (mut best_error, mut best_i) = (u32::MAX, 0usize);
    let (mut best_x, mut best_c1, mut best_c2) = (0u32, 0u32, 0u32);
    let out_of_range = |delta: i32| !(DELTA_MIN..=DELTA_MAX).contains(&delta);
    'search: for i in 0..3 {
        let (c1, c2) = (usize::from(color.c[NEXT_COMP[i]]), usize::from(color.c[NEXT_COMP[i + 1]]));
        for delta in -1..=1 {
            let c_plus_delta = (i32::from(color.c[i]) + delta).clamp(0, 255);
            for x in configs(c_plus_delta) {
                let diff = x & 1;
                if u32::from(use_diff) != diff {
                    continue;
                }
                if let (1, Some(base)) = (diff, base) {
                    if out_of_range(((x >> 8) & 255) as i32 - i32::from(base.c[i])) {
                        continue;
                    }
                }
                let table = &inverse[(x & 0xFF) as usize];
                let (p1, p2) = (u32::from(table[c1]), u32::from(table[c2]));
                if let (1, Some(base)) = (diff, base) {
                    if out_of_range((p1 & 0xFF) as i32 - i32::from(base.c[NEXT_COMP[i]]))
                        || out_of_range((p2 & 0xFF) as i32 - i32::from(base.c[NEXT_COMP[i + 1]]))
                    {
                        continue;
                    }
                }
                let trial = (c_plus_delta - i32::from(color.c[i])).pow(2) as u32 + (p1 >> 8).pow(2) + (p2 >> 8).pow(2);
                if trial < best_error {
                    best_error = trial;
                    best_x = x;
                    best_c1 = p1 & 0xFF;
                    best_c2 = p2 & 0xFF;
                    best_i = i;
                    if best_error == 0 {
                        break 'search;
                    }
                }
            }
        }
    }
    if best_error == u32::MAX {
        return;
    }
    results.color4 = best_x & 1 == 0;
    results.inten_table = (best_x >> 1) & 7;
    results.selectors = [((best_x >> 4) & 3) as u8; 8];
    results.color_unscaled.c[best_i] = ((best_x >> 8) & 255) as u8;
    results.color_unscaled.c[NEXT_COMP[best_i]] = best_c1 as u8;
    results.color_unscaled.c[NEXT_COMP[best_i + 1]] = best_c2 as u8;
    results.error = u64::from(best_error) * 8;
}

/// Packs a 4x4 block of RGBA texels (alpha 255, rows top first) into an ETC1 block, big-endian as rg_etc1 writes
/// it.
pub fn pack_block(texels: &[[u8; 4]; 16]) -> [u8; 8] {
    let pixels: [Color; 16] = texels.map(|c| Color { c });
    let mut block = [0u8; 8];
    if pixels.iter().all(|p| *p == pixels[0]) {
        pack_solid(&mut block, &pixels[0]);
        return block;
    }

    let mut best_error = u64::MAX;
    let (mut best_flip, mut best_use_color4) = (0u32, 0u32);
    let mut best_results = [Results::default(); 2];
    let mut results = [Results::default(); 3];
    let mut subblock_pixels = [Color::default(); 8];
    for flip in 0..2u32 {
        for use_color4 in 0..2u32 {
            let mut trial_error = 0u64;
            let mut completed = 0;
            for subblock in 0..2usize {
                if flip != 0 {
                    subblock_pixels.copy_from_slice(&pixels[subblock * 8..subblock * 8 + 8]);
                } else {
                    let column = &pixels[subblock * 2..];
                    subblock_pixels = [
                        column[0], column[4], column[8], column[12], column[1], column[5], column[9], column[13],
                    ];
                }
                results[2].error = u64::MAX;
                if (subblock != 0 || use_color4 != 0) && subblock_pixels.iter().all(|p| *p == subblock_pixels[0]) {
                    let base = (subblock != 0 && use_color4 == 0).then_some(results[0].color_unscaled);
                    pack_solid_constrained(&mut results[2], &subblock_pixels[0], use_color4 == 0, base.as_ref());
                }
                let mut params = Params {
                    pixels: &subblock_pixels,
                    use_color4: use_color4 != 0,
                    scan_deltas: &[-1, 0, 1],
                    base_color5: Color::default(),
                    constrain_against_base_color5: false,
                };
                if use_color4 == 0 && subblock != 0 {
                    params.constrain_against_base_color5 = true;
                    params.base_color5 = results[0].color_unscaled;
                }
                let mut optimizer = Optimizer::new(&params);
                let Some(mut found) = optimizer.compute(&params) else {
                    results[subblock].error = u64::from(u32::MAX);
                    break;
                };
                // Far from it: scan further away.
                if found.error > 3000 {
                    params.scan_deltas = &[-3, -2, 2, 3];
                    match optimizer.compute(&params) {
                        Some(better) => found = better,
                        None => break,
                    }
                }
                results[subblock] = found;
                if results[2].error < results[subblock].error {
                    results[subblock] = results[2];
                }
                trial_error += results[subblock].error;
                if trial_error >= best_error {
                    break;
                }
                completed = subblock + 1;
            }
            if completed < 2 {
                continue;
            }
            best_error = trial_error;
            best_results = [results[0], results[1]];
            best_flip = flip;
            best_use_color4 = use_color4;
        }
    }

    let (first, second) = (&best_results[0].color_unscaled, &best_results[1].color_unscaled);
    if best_use_color4 != 0 {
        for i in 0..3 {
            block[i] = second.c[i] | (first.c[i] << 4);
        }
    } else {
        for i in 0..3 {
            let mut d = i32::from(second.c[i]) - i32::from(first.c[i]);
            if d < 0 {
                d += 8;
            }
            block[i] = ((u32::from(first.c[i]) << 3) as u8) | d as u8;
        }
    }
    block[3] = ((best_results[1].inten_table << 2)
        | (best_results[0].inten_table << 5)
        | ((!best_use_color4 & 1) << 1)
        | best_flip) as u8;

    let (mut selector0, mut selector1) = (0u32, 0u32);
    let mut push = |s: u8| {
        let b = SELECTOR_INDEX_TO_ETC1[usize::from(s)];
        selector0 = (selector0 << 1) | (b & 1);
        selector1 = (selector1 << 1) | (b >> 1);
    };
    if best_flip != 0 {
        let (s0, s1) = (&best_results[0].selectors, &best_results[1].selectors);
        for x in (0..4).rev() {
            push(s1[4 + x]);
            push(s1[x]);
            push(s0[4 + x]);
            push(s0[x]);
        }
    } else {
        for subblock in (0..2).rev() {
            let s = &best_results[subblock].selectors;
            for half in [4, 0] {
                for k in (0..4).rev() {
                    push(s[half + k]);
                }
            }
        }
    }
    block[4] = (selector1 >> 8) as u8;
    block[5] = (selector1 & 0xFF) as u8;
    block[6] = (selector0 >> 8) as u8;
    block[7] = (selector0 & 0xFF) as u8;
    block
}
