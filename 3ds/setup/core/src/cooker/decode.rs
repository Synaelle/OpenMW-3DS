//! Texture files to RGBA, as Pillow decodes them (the cooker's reference).

use super::image::Image;
use crate::{fail, Result};

fn get32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

fn get16(p: &[u8]) -> u16 {
    u16::from_le_bytes([p[0], p[1]])
}

const DDPF_ALPHA_PIXELS: u32 = 0x1;
const DDPF_FOURCC: u32 = 0x4;
const DDPF_RGB: u32 = 0x40;
const DDPF_LUMINANCE: u32 = 0x20000;

// Pillow's BcnDecode.c.

fn decode565(x: u16) -> [u8; 4] {
    let x = u32::from(x);
    let mut r = (x & 0xf800) >> 8;
    r |= r >> 5;
    let mut g = (x & 0x7e0) >> 3;
    g |= g >> 6;
    let mut b = (x & 0x1f) << 3;
    b |= b >> 5;
    [r as u8, g as u8, b as u8, 0xff]
}

fn decode_bc1_color(dst: &mut [[u8; 4]; 16], src: &[u8], separate_alpha: bool) {
    let (c0, c1) = (get16(src), get16(&src[2..]));
    let mut p = [[0u8; 4]; 4];
    p[0] = decode565(c0);
    p[1] = decode565(c1);
    let [r0, g0, b0, _] = p[0].map(u32::from);
    let [r1, g1, b1, _] = p[1].map(u32::from);
    if c0 > c1 || separate_alpha {
        p[2] = [((2 * r0 + r1) / 3) as u8, ((2 * g0 + g1) / 3) as u8, ((2 * b0 + b1) / 3) as u8, 0xff];
        p[3] = [((r0 + 2 * r1) / 3) as u8, ((g0 + 2 * g1) / 3) as u8, ((b0 + 2 * b1) / 3) as u8, 0xff];
    } else {
        p[2] = [((r0 + r1) / 2) as u8, ((g0 + g1) / 2) as u8, ((b0 + b1) / 2) as u8, 0xff];
        p[3] = [0, 0, 0, 0];
    }
    for n in 0..4 {
        for o in 0..4 {
            dst[n * 4 + o] = p[usize::from(3 & (src[4 + n] >> (2 * o)))];
        }
    }
}

fn decode_bc3_alpha(dst: &mut [[u8; 4]; 16], src: &[u8]) {
    let (a0, a1) = (u32::from(src[0]), u32::from(src[1]));
    let lut1 = u32::from(src[2]) | u32::from(src[3]) << 8 | u32::from(src[4]) << 16;
    let lut2 = u32::from(src[5]) | u32::from(src[6]) << 8 | u32::from(src[7]) << 16;
    let mut a = [0u8; 8];
    a[0] = a0 as u8;
    a[1] = a1 as u8;
    if a0 > a1 {
        for i in 0..6 {
            a[2 + i] = (((6 - i as u32) * a0 + (1 + i as u32) * a1) / 7) as u8;
        }
    } else {
        for i in 0..4 {
            a[2 + i] = (((4 - i as u32) * a0 + (1 + i as u32) * a1) / 5) as u8;
        }
        a[6] = 0;
        a[7] = 0xff;
    }
    for n in 0..8 {
        dst[n][3] = a[(7 & (lut1 >> (3 * n))) as usize];
        dst[8 + n][3] = a[(7 & (lut2 >> (3 * n))) as usize];
    }
}

fn decode_dxt(data: &[u8], width: usize, height: usize, format: u32) -> Result<Image> {
    let mut image = Image::new(width, height);
    let block_bytes = if format == 1 { 8 } else { 16 };
    let (blocks_x, blocks_y) = (width.div_ceil(4), height.div_ceil(4));
    if data.len() < block_bytes * blocks_x * blocks_y {
        return fail("truncated DDS");
    }
    let mut block = [[0u8; 4]; 16];
    let mut src = data;
    for by in 0..blocks_y {
        for bx in 0..blocks_x {
            match format {
                1 => decode_bc1_color(&mut block, src, false),
                3 => {
                    decode_bc1_color(&mut block, &src[8..], true);
                    for (n, texel) in block.iter_mut().enumerate() {
                        let av = 0xf & (src[n * 4 >> 3] >> ((n * 4) & 7));
                        texel[3] = (av << 4) | av;
                    }
                }
                _ => {
                    decode_bc1_color(&mut block, &src[8..], true);
                    decode_bc3_alpha(&mut block, src);
                }
            }
            for y in 0..4 {
                for x in 0..4 {
                    let (px, py) = (bx * 4 + x, by * 4 + y);
                    if px < width && py < height {
                        let o = image.offset(px, py);
                        image.pixels[o..o + 4].copy_from_slice(&block[y * 4 + x]);
                    }
                }
            }
            src = &src[block_bytes..];
        }
    }
    Ok(image)
}

fn decode_dds(file: &[u8]) -> Result<Image> {
    if file.len() < 128 || &file[..4] != b"DDS " || get32(&file[4..]) != 124 {
        return fail("not a DDS file");
    }
    let header = &file[8..]; // after the magic and the header size
    let height = get32(&header[4..]) as usize;
    let width = get32(&header[8..]) as usize;
    let (pf_flags, four_cc, bit_count) = (get32(&header[72..]), get32(&header[76..]), get32(&header[80..]));
    let data = &file[128..];
    if pf_flags & DDPF_RGB != 0 {
        let alpha = pf_flags & DDPF_ALPHA_PIXELS != 0;
        let mask_count = if alpha { 4 } else { 3 };
        let mut masks = [0u32; 4];
        for (i, mask) in masks.iter_mut().enumerate().take(mask_count) {
            *mask = get32(&header[84 + 4 * i..]);
        }
        // Pillow's DdsRgbDecoder: each channel shifted down and scaled by a float division, truncated.
        let mut offsets = [0u32; 4];
        let mut totals = [0u32; 4];
        for i in 0..mask_count {
            let mut offset = 0;
            if masks[i] != 0 {
                while offset < 31 && ((masks[i] >> (offset + 1)) << (offset + 1)) == masks[i] {
                    offset += 1;
                }
            }
            offsets[i] = offset;
            totals[i] = masks[i] >> offset;
        }
        let bytes = (bit_count / 8) as usize;
        if bytes == 0 || bytes > 4 || data.len() < bytes * width * height {
            return fail("unsupported or truncated uncompressed DDS");
        }
        let mut image = Image::new(width, height);
        for (i, p) in data.chunks_exact(bytes).take(width * height).enumerate() {
            let mut value = 0u32;
            for (b, &byte) in p.iter().enumerate() {
                value |= u32::from(byte) << (8 * b);
            }
            let out = &mut image.pixels[i * 4..i * 4 + 4];
            for c in 0..mask_count {
                out[c] = if totals[c] != 0 {
                    ((f64::from((value & masks[c]) >> offsets[c]) / f64::from(totals[c])) * 255.0) as i32 as u8
                } else {
                    0
                };
            }
            if !alpha {
                out[3] = 255;
            }
        }
        return Ok(image);
    }
    if pf_flags & DDPF_LUMINANCE != 0 {
        let alpha = bit_count == 16 && pf_flags & DDPF_ALPHA_PIXELS != 0;
        if bit_count != 8 && !alpha {
            return fail("unsupported luminance DDS");
        }
        let bytes = if alpha { 2 } else { 1 };
        if data.len() < bytes * width * height {
            return fail("truncated DDS");
        }
        let mut image = Image::new(width, height);
        for i in 0..width * height {
            let l = data[i * bytes];
            let a = if alpha { data[i * bytes + 1] } else { 255 };
            image.pixels[i * 4..i * 4 + 4].copy_from_slice(&[l, l, l, a]);
        }
        return Ok(image);
    }
    if pf_flags & DDPF_FOURCC != 0 {
        let format = match four_cc {
            0x31545844 => 1, // DXT1
            0x33545844 => 3, // DXT3
            0x35545844 => 5, // DXT5
            _ => return fail("unsupported DDS compression"),
        };
        return decode_dxt(data, width, height, format);
    }
    fail("unsupported DDS pixel format")
}

/// A PNG as stb_image decodes it: palettes and grey expanded, 16-bit channels cut to their high byte.
fn decode_png(file: &[u8]) -> Result<Image> {
    let mut decoder = png::Decoder::new(file);
    decoder.set_transformations(png::Transformations::EXPAND);
    let mut reader = decoder.read_info().map_err(|e| crate::Error::Failed(format!("can't decode: {e}")))?;
    let mut buffer = vec![0; reader.output_buffer_size()];
    let info = reader.next_frame(&mut buffer).map_err(|e| crate::Error::Failed(format!("can't decode: {e}")))?;
    let (width, height) = (info.width as usize, info.height as usize);
    let channels = info.color_type.samples();
    let wide = info.bit_depth == png::BitDepth::Sixteen;
    let mut image = Image::new(width, height);
    for y in 0..height {
        let line = &buffer[y * info.line_size..(y + 1) * info.line_size];
        for x in 0..width {
            let sample = |c: usize| if wide { line[(x * channels + c) * 2] } else { line[x * channels + c] };
            let rgba = match channels {
                1 => [sample(0), sample(0), sample(0), 255],
                2 => [sample(0), sample(0), sample(0), sample(1)],
                3 => [sample(0), sample(1), sample(2), 255],
                _ => [sample(0), sample(1), sample(2), sample(3)],
            };
            let o = image.offset(x, y);
            image.pixels[o..o + 4].copy_from_slice(&rgba);
        }
    }
    Ok(image)
}

/// TGA, BMP and JPEG (none in Morrowind's own files: mods may have them).
fn decode_other(file: &[u8], format: image::ImageFormat) -> Result<Image> {
    let decoded = image::load_from_memory_with_format(file, format)
        .map_err(|e| crate::Error::Failed(format!("can't decode: {e}")))?
        .into_rgba8();
    Ok(Image { width: decoded.width() as usize, height: decoded.height() as usize, pixels: decoded.into_raw() })
}

/// Decodes a texture file (.dds, .tga, .bmp, .png, .jpg) to RGBA as Pillow does: DDS DXT1/DXT3/DXT5, uncompressed
/// (by its channel masks) and luminance. `name` gives the format by its extension.
pub fn decode_image(data: &[u8], name: &str) -> Result<Image> {
    let lower = name.to_ascii_lowercase();
    let extension = lower.rsplit('.').next().unwrap_or_default();
    match extension {
        "dds" => decode_dds(data),
        "png" => decode_png(data),
        "tga" => decode_other(data, image::ImageFormat::Tga),
        "bmp" => decode_other(data, image::ImageFormat::Bmp),
        _ => decode_other(data, image::ImageFormat::Jpeg),
    }
}

/// An uncompressed DDS (no FourCC): the cooker keeps small ones in RGBA8 (ETC1 artifacts show on them).
pub fn dds_is_uncompressed(data: &[u8]) -> bool {
    data.len() >= 88 && &data[..4] == b"DDS " && get32(&data[84..]) == 0
}

/// A 16-bit TGA with one attribute bit: Morrowind ignores that alpha (as OpenMW's ImageManager does).
pub fn tga_ignores_alpha(data: &[u8]) -> bool {
    if data.len() < 18 {
        return false;
    }
    let image_type = data[2];
    let depth = if image_type == 1 || image_type == 9 { data[7] } else { data[16] };
    depth == 16 && (data[17] & 0x0F) == 1
}
