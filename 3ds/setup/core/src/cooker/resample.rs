//! Image.resize as Pillow 12 does it (the cooker's reference): Pillow's libImaging/Resample.c and the RGBA <-> RGBa
//! conversions of Convert.c, kept to the same arithmetic so the results are the same bytes. RGBA premultiplied by
//! alpha around the resampling, 22-bit fixed-point coefficients, the horizontal pass first, a pass only along an
//! axis whose size changes.

use super::image::Image;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Filter {
    Bicubic,
    Lanczos,
}

const PRECISION_BITS: i32 = 32 - 8 - 2;

fn bicubic(x: f64) -> f64 {
    let a = -0.5;
    let x = x.abs();
    if x < 1.0 {
        ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0
    } else if x < 2.0 {
        (((x - 5.0) * x + 8.0) * x - 4.0) * a
    } else {
        0.0
    }
}

fn sinc(x: f64) -> f64 {
    if x == 0.0 {
        return 1.0;
    }
    let x = x * std::f64::consts::PI;
    x.sin() / x
}

fn lanczos(x: f64) -> f64 {
    if (-3.0..3.0).contains(&x) { sinc(x) * sinc(x / 3.0) } else { 0.0 }
}

struct Coefficients {
    ksize: usize,
    bounds: Vec<(usize, usize)>, // xmin, count per output texel
    k: Vec<i32>,                 // ksize per output texel
}

fn precompute(in_size: usize, out_size: usize, filter: Filter) -> Coefficients {
    let (function, filter_support): (fn(f64) -> f64, f64) = match filter {
        Filter::Lanczos => (lanczos, 3.0),
        Filter::Bicubic => (bicubic, 2.0),
    };
    let (in0, in1) = (0f32, in_size as f32);
    let scale = f64::from(in1 - in0) / out_size as f64;
    let filterscale = if scale < 1.0 { 1.0 } else { scale };
    let support = filter_support * filterscale;
    let ksize = support.ceil() as usize * 2 + 1;
    let mut bounds = Vec::with_capacity(out_size);
    let mut kk = vec![0f64; out_size * ksize];
    let inv_filterscale = 1.0 / filterscale;
    for xx in 0..out_size {
        let center = f64::from(in0) + (xx as f64 + 0.5) * scale;
        let mut ww = 0.0;
        let xmin = ((center - support + 0.5) as i32).max(0);
        let xmax = ((center + support + 0.5) as i32).min(in_size as i32) - xmin;
        let k = &mut kk[xx * ksize..(xx + 1) * ksize];
        for x in 0..xmax.max(0) {
            let w = function((f64::from(x + xmin) - center + 0.5) * inv_filterscale);
            k[x as usize] = w;
            ww += w;
        }
        if ww != 0.0 {
            for value in k.iter_mut().take(xmax.max(0) as usize) {
                *value /= ww;
            }
        }
        bounds.push((xmin as usize, xmax.max(0) as usize));
    }
    let k = kk
        .iter()
        .map(|&v| {
            let scaled = v * f64::from(1 << PRECISION_BITS);
            if v < 0.0 { (-0.5 + scaled) as i32 } else { (0.5 + scaled) as i32 }
        })
        .collect();
    Coefficients { ksize, bounds, k }
}

fn clip8(value: i32) -> u8 {
    (value >> PRECISION_BITS).clamp(0, 255) as u8
}

const HALF: i32 = 1 << (PRECISION_BITS - 1);

fn horizontal(input: &Image, out_width: usize, filter: Filter) -> Image {
    let c = precompute(input.width, out_width, filter);
    let mut out = Image::new(out_width, input.height);
    for y in 0..input.height {
        let line = input.row(y);
        for xx in 0..out_width {
            let (xmin, xmax) = c.bounds[xx];
            let k = &c.k[xx * c.ksize..];
            let mut ss = [HALF; 4];
            for x in 0..xmax {
                for (ch, s) in ss.iter_mut().enumerate() {
                    *s = s.wrapping_add(i32::from(line[(x + xmin) * 4 + ch]).wrapping_mul(k[x]));
                }
            }
            let o = out.offset(xx, y);
            for ch in 0..4 {
                out.pixels[o + ch] = clip8(ss[ch]);
            }
        }
    }
    out
}

fn vertical(input: &Image, out_height: usize, filter: Filter) -> Image {
    let c = precompute(input.height, out_height, filter);
    let mut out = Image::new(input.width, out_height);
    for yy in 0..out_height {
        let (ymin, ymax) = c.bounds[yy];
        let k = &c.k[yy * c.ksize..];
        for xx in 0..input.width {
            let mut ss = [HALF; 4];
            for y in 0..ymax {
                let p = input.at(xx, y + ymin);
                for (ch, s) in ss.iter_mut().enumerate() {
                    *s = s.wrapping_add(i32::from(p[ch]).wrapping_mul(k[y]));
                }
            }
            let o = out.offset(xx, yy);
            for ch in 0..4 {
                out.pixels[o + ch] = clip8(ss[ch]);
            }
        }
    }
    out
}

/// Convert.c's MULDIV255.
fn mul_div255(a: u32, b: u32) -> u8 {
    let tmp = a * b + 128;
    (((tmp >> 8) + tmp) >> 8) as u8
}

/// Resizes as Pillow's Image.resize does.
pub fn resize(image: &Image, width: usize, height: usize, filter: Filter) -> Image {
    if width == image.width && height == image.height {
        return image.clone();
    }
    // RGBA -> RGBa
    let mut work = Image::new(image.width, image.height);
    for (out, p) in work.pixels.chunks_exact_mut(4).zip(image.pixels.chunks_exact(4)) {
        let alpha = u32::from(p[3]);
        out[0] = mul_div255(u32::from(p[0]), alpha);
        out[1] = mul_div255(u32::from(p[1]), alpha);
        out[2] = mul_div255(u32::from(p[2]), alpha);
        out[3] = p[3];
    }
    if image.height > image.width * 100 && height < image.height {
        // Image.resize's very tall images: the height first, then the width (two resizes of the RGBa image).
        if height != image.height {
            work = vertical(&work, height, filter);
        }
        if width != image.width {
            work = horizontal(&work, width, filter);
        }
    } else {
        if width != image.width {
            work = horizontal(&work, width, filter);
        }
        if height != image.height {
            work = vertical(&work, height, filter);
        }
    }
    // RGBa -> RGBA
    for p in work.pixels.chunks_exact_mut(4) {
        let alpha = u32::from(p[3]);
        if alpha != 255 && alpha != 0 {
            for value in &mut p[..3] {
                *value = ((255 * u32::from(*value)) / alpha).min(255) as u8;
            }
        }
    }
    work
}
