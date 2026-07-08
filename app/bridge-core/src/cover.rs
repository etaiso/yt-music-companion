//! Fetch + resize + RGB565-encode cover art (ports cover.js). The board can't
//! decode JPEG/PNG or resize, so we do it and emit a binary frame it blits
//! straight into an lv_image_dsc_t.
//!
//! Frame layout (LE): "YC"(2) | ver=1(u8) | fmt=0(u8) | w(u16) | h(u16) | pixels(w*h*2 RGB565 LE)
use crate::config::COVER_PX;

pub const COVER_HEADER_BYTES: usize = 8;

pub fn encode_rgb565(rgb: &[u8], width: u16, height: u16) -> Vec<u8> {
    let px = width as usize * height as usize;
    let mut out = Vec::with_capacity(COVER_HEADER_BYTES + px * 2);
    out.extend_from_slice(b"YC");
    out.push(1); // version
    out.push(0); // format: RGB565 LE
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&height.to_le_bytes());
    for chunk in rgb.chunks_exact(3) {
        let (r, g, b) = (chunk[0], chunk[1], chunk[2]);
        let v: u16 = (((r as u16) & 0xF8) << 8) | (((g as u16) & 0xFC) << 3) | ((b as u16) >> 3);
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}

/// Fetch `url`, square-crop to COVER_PX (cover-fit, centre), return the RGB565
/// frame. Returns `None` on any failure (board keeps its gradient placeholder).
pub async fn render_cover(url: &str) -> Option<Vec<u8>> {
    if url.is_empty() {
        return None;
    }
    match render_cover_inner(url).await {
        Ok(frame) => Some(frame),
        Err(e) => {
            tracing::error!("[cover] render failed ({url}): {e}");
            None
        }
    }
}

async fn render_cover_inner(url: &str) -> anyhow::Result<Vec<u8>> {
    let bytes = reqwest::get(url).await?.error_for_status()?.bytes().await?;
    let img = image::load_from_memory(&bytes)?;
    // cover-fit centre crop to a COVER_PX square, then drop alpha -> RGB8.
    let filled = img.resize_to_fill(COVER_PX, COVER_PX, image::imageops::FilterType::Lanczos3);
    let rgb = filled.to_rgb8();
    Ok(encode_rgb565(rgb.as_raw(), COVER_PX as u16, COVER_PX as u16))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_is_well_formed() {
        // 1x1 pure red: r=255,g=0,b=0 -> 0xF800
        let frame = encode_rgb565(&[255, 0, 0], 1, 1);
        assert_eq!(&frame[0..2], b"YC");
        assert_eq!(frame[2], 1); // version
        assert_eq!(frame[3], 0); // format RGB565 LE
        assert_eq!(u16::from_le_bytes([frame[4], frame[5]]), 1); // width
        assert_eq!(u16::from_le_bytes([frame[6], frame[7]]), 1); // height
        assert_eq!(frame.len(), COVER_HEADER_BYTES + 1 * 1 * 2);
        // pixel: 0xF800 little-endian -> [0x00, 0xF8]
        assert_eq!(frame[8], 0x00);
        assert_eq!(frame[9], 0xF8);
    }

    #[test]
    fn packs_green_and_blue_channels() {
        // pure green 0x07E0 -> LE [0xE0,0x07]; pure blue 0x001F -> LE [0x1F,0x00]
        let frame = encode_rgb565(&[0, 255, 0, 0, 0, 255], 2, 1);
        assert_eq!(&frame[8..10], &[0xE0, 0x07]);
        assert_eq!(&frame[10..12], &[0x1F, 0x00]);
    }

    #[test]
    fn length_matches_dimensions() {
        let px = 4 * 4;
        let frame = encode_rgb565(&vec![0u8; px * 3], 4, 4);
        assert_eq!(frame.len(), COVER_HEADER_BYTES + px * 2);
    }
}
