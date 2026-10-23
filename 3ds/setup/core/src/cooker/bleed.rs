//! Colouring the fully transparent texels.

use super::image::Image;

/// numpy's pairwise summation of a float32 array (the 1-D sum of the alpha weights).
fn pairwise_sum(a: &[f32]) -> f32 {
    let n = a.len();
    if n < 8 {
        let mut res = 0f32;
        for &v in a {
            res += v;
        }
        return res;
    }
    if n <= 128 {
        let mut r = [0f32; 8];
        r.copy_from_slice(&a[..8]);
        let mut i = 8;
        while i < n - (n % 8) {
            for j in 0..8 {
                r[j] += a[i + j];
            }
            i += 8;
        }
        let mut res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        while i < n {
            res += a[i];
            i += 1;
        }
        return res;
    }
    let mut n2 = n / 2;
    n2 -= n2 % 8;
    pairwise_sum(&a[..n2]) + pairwise_sum(&a[n2..])
}

/// Colours the fully transparent texels with the alpha-weighted colour of the visible texels nearby (up to 8 texels
/// away, wrapping like a repeating texture), then the texture's average colour. The encoder replaces the colour of
/// alpha-0 texels with black, which bilinear filtering and mipmaps blend into the visible edges (dark fringes, black
/// raindrops). Unless `keep_alpha_zero` (RGBA8, where alpha 1 would show), those texels get alpha 1 instead, which
/// keeps their colour and still encodes as 0 in ETC1A4's 4-bit alpha.
/// The arithmetic follows the Python cooker's numpy code in float32, operation for operation.
pub fn bleed_transparent(image: &Image, keep_alpha_zero: bool) -> Image {
    let (w, h) = (image.width, image.height);
    let n = w * h;
    let clear: Vec<bool> = (0..n).map(|i| image.pixels[i * 4 + 3] == 0).collect();
    let clear_count = clear.iter().filter(|&&c| c).count();
    if clear_count == 0 || clear_count == n {
        return image.clone();
    }

    let mut weight: Vec<f32> = (0..n).map(|i| f32::from(image.pixels[i * 4 + 3]) / 255.0).collect();
    let mut color = vec![0f32; n * 3];
    for i in 0..n {
        for c in 0..3 {
            color[i * 3 + c] = f32::from(image.pixels[i * 4 + c]) * weight[i];
        }
    }
    let mut filled: Vec<bool> = clear.iter().map(|&c| !c).collect();

    let mut color_sum = vec![0f32; n * 3];
    let mut weight_sum = vec![0f32; n];
    let mut grow = vec![false; n];
    for _ in 0..8 {
        if filled.iter().all(|&f| f) {
            break;
        }
        color_sum.fill(0.0);
        weight_sum.fill(0.0);
        // np.roll(a, (dy, dx)): out[y][x] = a[y - dy][x - dx], wrapping; summed in the loops' order.
        for dy in -1i64..=1 {
            for dx in -1i64..=1 {
                for y in 0..h {
                    let sy = (y as i64 - dy).rem_euclid(h as i64) as usize;
                    for x in 0..w {
                        let sx = (x as i64 - dx).rem_euclid(w as i64) as usize;
                        let (o, s) = (y * w + x, sy * w + sx);
                        for c in 0..3 {
                            color_sum[o * 3 + c] += color[s * 3 + c];
                        }
                        weight_sum[o] += weight[s];
                    }
                }
            }
        }
        let mut any = false;
        for i in 0..n {
            grow[i] = !filled[i] && weight_sum[i] > 0.0;
            any |= grow[i];
        }
        if !any {
            break;
        }
        for i in 0..n {
            if !grow[i] {
                continue;
            }
            let new_weight = weight_sum[i] / 18.0;
            for c in 0..3 {
                color[i * 3 + c] = color_sum[i * 3 + c] / weight_sum[i] * new_weight;
            }
            weight[i] = new_weight;
            filled[i] = true;
        }
    }

    // The visible texels' alpha-weighted mean colour, for the texels nothing reached: the colour sums run
    // sequentially (numpy reduces along the outer axis), the weights' sum pairwise (a 1-D reduction).
    let mut mean_sum = [0f32; 3];
    let mut visible_weights = Vec::with_capacity(n - clear_count);
    for i in 0..n {
        if clear[i] {
            continue;
        }
        let a = f32::from(image.pixels[i * 4 + 3]) / 255.0;
        visible_weights.push(a);
        for c in 0..3 {
            mean_sum[c] += f32::from(image.pixels[i * 4 + c]) * a;
        }
    }
    let total = pairwise_sum(&visible_weights).max(1e-6);
    let mean = mean_sum.map(|s| s / total);

    let mut out = image.clone();
    for i in 0..n {
        if !clear[i] {
            continue;
        }
        for c in 0..3 {
            let v = if filled[i] { color[i * 3 + c] / weight[i].max(1e-6) } else { mean[c] };
            out.pixels[i * 4 + c] = v.round_ties_even().clamp(0.0, 255.0) as u8;
        }
        if !keep_alpha_zero {
            out.pixels[i * 4 + 3] = 1;
        }
    }
    out
}
