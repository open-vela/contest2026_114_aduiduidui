#!/usr/bin/env python3
"""预览: 读取 fox_pixel_gen.py 生成的逻辑, 输出 8 张 PNG"""
import math, struct, os
from PIL import Image

W, H = 80, 80
BG = (30, 30, 30)

# 复用主脚本的绘制逻辑 (内联)
def rgb565_to_tuple(lo, hi):
    """little-endian bytes → (r,g,b)"""
    v = (hi << 8) | lo
    return ((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3

# 读 fox_images.h 解析像素
def parse_image(fpath):
    with open(fpath, 'r', encoding='utf-8') as f:
        content = f.read()
    images = []
    for level in range(8):
        # 找数组起始
        marker = f"img_fox_level_{level}_data[FOX_IMG_SZ] = {{"
        start = content.index(marker) + len(marker)
        end = content.index("};", start)
        block = content[start:end]
        # 解析 hex 值
        vals = []
        for token in block.split(","):
            token = token.strip()
            if token.startswith("0x") or token.startswith("0X"):
                vals.append(int(token, 16))
        # 转像素
        pixels = []
        for i in range(0, len(vals), 2):
            pixels.append(rgb565_to_tuple(vals[i], vals[i+1]))
        images.append(pixels)
    return images

script_dir = os.path.dirname(os.path.abspath(__file__))
h_path = os.path.join(script_dir, "fox_images.h")
imgs = parse_image(h_path)

# 合成为一张 8 列图
out = Image.new("RGB", (W * 8 + 9*4, H + 40), (40,40,40))
from PIL import ImageDraw, ImageFont
draw = ImageDraw.Draw(out)
names = ["LV0 初始狐","LV1 捧星狐","LV2 披风狐","LV3 捧书狐",
         "LV4 星披狐","LV5 书披狐","LV6 王冠狐","LV7 究极狐"]
for i, px in enumerate(imgs):
    img = Image.new("RGB", (W, H))
    img.putdata(px)
    x = 4 + i * (W + 4)
    out.paste(img, (x, 28))
    draw.text((x + W//2 - 20, 6), names[i], fill=(220,220,220))

out_path = os.path.join(script_dir, "fox_preview.png")
out.save(out_path)
print(f"Preview saved: {out_path}")
