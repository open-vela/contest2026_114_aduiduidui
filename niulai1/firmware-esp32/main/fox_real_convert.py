#!/usr/bin/env python3
"""Convert fox PNGs -> 130x130 RGB565, natural color, PRE-SWAPPED bytes (LV_COLOR_16_SWAP=1) -> fox_images.h"""

from PIL import Image
import os

W, H = 130, 130

FILES = [
    (0, "C:/Users/21595/Desktop/微信图片_20260803050051_468_2.png"),
    (1, "C:/Users/21595/Desktop/微信图片_20260803050053_469_2.png"),
    (2, "C:/Users/21595/Desktop/微信图片_20260803050054_470_2.png"),
    (3, "C:/Users/21595/Desktop/微信图片_20260803050055_471_2.png"),
    (4, "C:/Users/21595/Desktop/微信图片_20260803050056_472_2.png"),
    (5, "C:/Users/21595/Desktop/微信图片_20260803050057_473_2.png"),
    (6, "C:/Users/21595/Desktop/微信图片_20260803050058_474_2.png"),
    (7, "C:/Users/21595/Desktop/微信图片_20260803050059_475_2.png"),
]

def fox_pixel(r, g, b):
    """Keep the fox's natural color (no R/B swap, no color boost).

    The old version swapped R/B and applied non-linear gains to compensate for a
    byte-order bug. That bug is actually fixed by emitting PRE-SWAPPED RGB565
    bytes in rgb565() below: LVGL is built with LV_COLOR_16_SWAP=1 and blits
    LV_IMG_CF_TRUE_COLOR image data straight into the frame buffer without any
    byte swap, so the C array must already be high-byte-first. Colors therefore
    stay natural here and must NOT be channel-swapped or re-gained."""
    return (r, g, b)

def process_img(path):
    """Composite onto the dark screen background; fox colors kept natural"""
    src = Image.open(path).convert("RGBA")
    w, h = src.size
    spx = src.load()
    # Find bg color from opaque edge pixels
    samples = []
    for x in range(0, w, 10):
        for y in [0, 1, 2, h-3, h-2, h-1]:
            r, g, b, a = spx[x, y]
            if a > 128: samples.append((r, g, b))
        for y in range(0, h, 10):
            for x in [0, 1, 2, w-3, w-2, w-1]:
                r, g, b, a = spx[x, y]
                if a > 128: samples.append((r, g, b))
    from collections import Counter
    bg = (16, 16, 16)  # match screen bg #101010
    print(f"  bg=RGB{bg}")
    out = Image.new("RGBA", src.size, (*bg, 255))
    opx = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = spx[x, y]
            if a > 128:
                opx[x, y] = (*fox_pixel(r, g, b), 255)
    return out.resize((W, H), Image.LANCZOS).convert("RGB")

def rgb565(r, g, b):
    """RGB565 encoding, emitted PRE-SWAPPED (high byte first).

    LVGL is built with LV_COLOR_16_SWAP=1 and copies LV_IMG_CF_TRUE_COLOR pixel
    data straight into the frame buffer without swapping, so the array bytes
    must already be in the high-byte-first order the ST7789 expects over SPI."""
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return f"0x{v>>8:02X}, 0x{v&0xFF:02X}"

def img_to_c(img):
    px = list(img.getdata())
    lines = []
    for y in range(H):
        row = [rgb565(*px[y*W+x]) for x in range(W)]
        lines.append("    " + ", ".join(row))
    return ",\n".join(lines)

datas = {}
for lv, path in FILES:
    print(f"LV_{lv}: {os.path.basename(path)}")
    img = process_img(path)
    img.save(f"fox_lv_{lv}_preview.png")
    datas[lv] = img_to_c(img)

out = []
out.append("""#ifndef FOX_IMAGES_H
#define FOX_IMAGES_H
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
#define FOX_IMG_W 130
#define FOX_IMG_H 130
#define FOX_IMG_SZ (130*130*2)
""")

for n in range(8):
    out.append(f"static const uint8_t img_fox_level_{n}_data[FOX_IMG_SZ] = {{")
    out.append(datas[n])
    out.append("};")
    out.append(f"const lv_img_dsc_t img_fox_level_{n} = {{")
    out.append(f"    .header = {{ .cf = LV_IMG_CF_TRUE_COLOR, .always_zero = 0, .reserved = 0, .w = FOX_IMG_W, .h = FOX_IMG_H }},")
    out.append(f"    .data_size = FOX_IMG_SZ,")
    out.append(f"    .data      = img_fox_level_{n}_data")
    out.append(f"}};")

out.append("const lv_img_dsc_t *const g_fox_img_table[8] = {")
for n in range(8):
    out.append(f"    [FOX_LV_{n}] = &img_fox_level_{n},")
out.append("};")
out.append("#ifdef __cplusplus\n}\n#endif\n#endif")

with open("fox_images.h", "w", encoding="utf-8") as f:
    f.write("\n".join(out))
print(f"fox_images.h: {len(''.join(out)):,} bytes ({W}x{H}x8 RGB565, natural color, pre-swapped bytes)")
