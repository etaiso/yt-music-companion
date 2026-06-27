// cover.js — fetch + resize + RGB565 encode of cover art (SPEC §4.4).
//
// The board can't decode JPEG/PNG or resize, so the bridge does it: fetch the
// best thumbnail, square-crop to the cover slot, convert to RGB565, and emit a
// binary frame the board can blit straight into an lv_image_dsc_t.
//
// Binary frame layout (little-endian):
//   off 0  : "YC"          magic (2 bytes)
//   off 2  : version = 1    (u8)
//   off 3  : format  = 0    (u8) -> RGB565, low byte first (LVGL native LE)
//   off 4  : width         (u16)
//   off 6  : height        (u16)
//   off 8  : pixels        width*height*2 bytes, RGB565 LE
import sharp from "sharp";
import { COVER_PX } from "./config.js";

const HEADER_BYTES = 8;

function encodeRgb565(rgb, width, height) {
  const out = Buffer.allocUnsafe(HEADER_BYTES + width * height * 2);
  out.write("YC", 0, "ascii");
  out.writeUInt8(1, 2); // version
  out.writeUInt8(0, 3); // format: RGB565 LE
  out.writeUInt16LE(width, 4);
  out.writeUInt16LE(height, 6);

  let p = HEADER_BYTES;
  for (let i = 0; i < rgb.length; i += 3) {
    const r = rgb[i];
    const g = rgb[i + 1];
    const b = rgb[i + 2];
    const v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
    out.writeUInt16LE(v, p);
    p += 2;
  }
  return out;
}

// Fetch `url`, square-crop to COVER_PX, return the RGB565 binary frame.
// Returns null on any failure (board keeps its gradient placeholder).
export async function renderCover(url) {
  if (!url) return null;
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const input = Buffer.from(await res.arrayBuffer());

    const { data, info } = await sharp(input)
      .resize(COVER_PX, COVER_PX, { fit: "cover", position: "centre" })
      .removeAlpha() // 3 channels, no alpha
      .raw()
      .toBuffer({ resolveWithObject: true });

    return encodeRgb565(data, info.width, info.height);
  } catch (err) {
    console.error(`[cover] render failed (${url}): ${err.message}`);
    return null;
  }
}

export const COVER_HEADER_BYTES = HEADER_BYTES;
