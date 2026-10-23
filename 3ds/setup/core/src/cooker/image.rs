//! 8-bit RGBA images, top row first.

#[derive(Clone, Default)]
pub struct Image {
    pub width: usize,
    pub height: usize,
    pub pixels: Vec<u8>,
}

impl Image {
    pub fn new(width: usize, height: usize) -> Self {
        Image { width, height, pixels: vec![0; width * height * 4] }
    }

    pub fn offset(&self, x: usize, y: usize) -> usize {
        (y * self.width + x) * 4
    }

    pub fn at(&self, x: usize, y: usize) -> &[u8] {
        let o = self.offset(x, y);
        &self.pixels[o..o + 4]
    }

    pub fn row(&self, y: usize) -> &[u8] {
        &self.pixels[y * self.width * 4..(y + 1) * self.width * 4]
    }

    pub fn row_mut(&mut self, y: usize) -> &mut [u8] {
        let width = self.width;
        &mut self.pixels[y * width * 4..(y + 1) * width * 4]
    }
}

pub fn flip_vertical(image: &Image) -> Image {
    let mut out = Image::new(image.width, image.height);
    for y in 0..image.height {
        out.row_mut(image.height - 1 - y).copy_from_slice(image.row(y));
    }
    out
}

pub fn transposed(image: &Image) -> Image {
    let mut out = Image::new(image.height, image.width);
    for y in 0..image.height {
        for x in 0..image.width {
            let o = out.offset(y, x);
            out.pixels[o..o + 4].copy_from_slice(image.at(x, y));
        }
    }
    out
}

/// The top-left w x h of the image.
pub fn cropped(image: &Image, w: usize, h: usize) -> Image {
    let mut out = Image::new(w, h);
    for y in 0..h {
        out.row_mut(y).copy_from_slice(&image.row(y)[..w * 4]);
    }
    out
}

/// Grows the image to w x h by repeating its last column and row (numpy's pad mode "edge").
pub fn padded_edge(image: &Image, w: usize, h: usize) -> Image {
    padded_edge_around(image, 0, 0, w - image.width, h - image.height)
}

/// Pads with `left`/`top`/`right`/`bottom` texels repeating the edge on each side.
pub fn padded_edge_around(image: &Image, left: usize, top: usize, right: usize, bottom: usize) -> Image {
    let mut out = Image::new(image.width + left + right, image.height + top + bottom);
    for y in 0..out.height {
        let sy = y.saturating_sub(top).min(image.height - 1);
        for x in 0..out.width {
            let sx = x.saturating_sub(left).min(image.width - 1);
            let o = out.offset(x, y);
            out.pixels[o..o + 4].copy_from_slice(image.at(sx, sy));
        }
    }
    out
}
