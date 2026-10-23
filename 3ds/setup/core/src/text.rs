//! Text helpers for the game's files: Windows-1252, ASCII case, trimming.

/// Windows-1252's 0x80-0x9F block (the rest of the upper half is Latin-1, the same code points).
const HIGH_BLOCK: [u16; 32] = [
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, //
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, //
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, //
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, //
];

/// Windows-1252 (the encoding of English and Western Morrowind files) to UTF-8.
pub fn cp1252_to_utf8(text: &[u8]) -> String {
    text.iter()
        .map(|&byte| match byte {
            0x80..=0x9F => char::from_u32(u32::from(HIGH_BLOCK[usize::from(byte - 0x80)])).unwrap_or('?'),
            _ => char::from(byte),
        })
        .collect()
}

/// Lower case, ASCII only (game file names).
pub fn to_lower_ascii(text: &str) -> String {
    text.to_ascii_lowercase()
}

/// Without the spaces, tabs and line ends around it.
pub fn trim(text: &[u8]) -> &[u8] {
    let is_space = |c: &u8| matches!(c, b' ' | b'\t' | b'\r' | b'\n');
    let start = text.iter().position(|c| !is_space(c)).unwrap_or(text.len());
    let end = text.iter().rposition(|c| !is_space(c)).map_or(start, |i| i + 1);
    &text[start..end]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn converts_cp1252() {
        assert_eq!(cp1252_to_utf8(b"caf\xE9 \x80"), "café €");
    }

    #[test]
    fn trims() {
        assert_eq!(trim(b"  a b\r\n"), b"a b");
        assert_eq!(trim(b" \t "), b"");
    }
}
