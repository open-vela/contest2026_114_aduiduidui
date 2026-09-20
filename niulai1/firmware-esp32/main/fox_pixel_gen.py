#!/usr/bin/env python3
"""
fox_pixel_gen.py — 130×130 RGB565 小狐狸像素生成器 (Floyd-Steinberg 抖动版)
============================================================================
生成 8 阶段狐狸图像 (LV_0 ~ LV_7) 的 RGB565 C 数组, 输出到 fox_images.h。

关键改进:
  1. 废弃固定色板"映射匹配原图"的逻辑 — 改为程序化几何绘制 + 抖动量化
  2. Floyd-Steinberg 误差扩散: 绘制完成后对画布逐像素抖动, 消除色带/马赛克
  3. 背景像素强制锁定 (16, 16, 16) = #101010, 与 LVGL 屏幕底色完全融合

用法: python fox_pixel_gen.py
输出: fox_images.h (覆盖)

依赖: Python 3.7+, PIL (Pillow)
"""

import math, os

W, H = 130, 130
BG = (16, 16, 16)       # 屏幕底色 #101010, 必须精确


# ═══════════════════════════════════════════════════════════════
#  RGB565 工具
# ═══════════════════════════════════════════════════════════════

def rgb888_to_565(r, g, b):
    """8-bit RGB → 16-bit RGB565 integer"""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rgb565_to_888(v):
    """16-bit RGB565 → 8-bit (r,g,b) tuple (重建值, 有量化误差)"""
    r = ((v >> 11) & 0x1F) << 3
    g = ((v >> 5)  & 0x3F) << 2
    b = (v & 0x1F) << 3
    return (r, g, b)


def rgb565_to_be_bytes(v):
    """RGB565 → big-endian hex string pair (高字节在前)。

    LVGL 以 LV_COLOR_16_SWAP=1 构建, 对 LV_IMG_CF_TRUE_COLOR 图片走快速路径
    直接 blit、不做字节交换, 因此 C 数组必须预先按高字节在前存储, 才能匹配
    ST7789 通过 SPI 期望的字节序。旧版 (小端) 会导致每像素两字节对调、
    颜色错乱 (屏幕发红发白)。
    """
    return f"0x{v >> 8:02X}, 0x{v & 0xFF:02X}"


# ═══════════════════════════════════════════════════════════════
#  绘制颜色 (程序化绘图的调色板, 非"原图匹配映射")
# ═══════════════════════════════════════════════════════════════

ORANGE    = (255, 107, 44)
ORANGE_D  = (220, 80, 20)
WHITE     = (255, 255, 255)
CREAM     = (255, 240, 225)
BLACK     = (20, 18, 20)
DARKBROWN = (60, 30, 15)
PINK      = (255, 143, 175)
PINK_D    = (230, 110, 140)
GOLD      = (255, 215, 0)
GOLD_D    = (220, 180, 0)
YELLOW    = (255, 184, 0)
YELLOW_D  = (220, 150, 0)
RED       = (200, 50, 50)
RED_D     = (160, 30, 30)
WHITEBG   = (255, 252, 248)
GREEN     = (80, 180, 80)
GEM_RED   = (240, 50, 60)


# ═══════════════════════════════════════════════════════════════
#  画布
# ═══════════════════════════════════════════════════════════════

class Canvas:
    def __init__(self):
        self.px = [[BG for _ in range(W)] for _ in range(H)]

    def put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[y][x] = c

    def put_aa(self, x, y, c, alpha):
        if 0 <= x < W and 0 <= y < H:
            a = max(0, min(255, alpha))
            bg = self.px[y][x]
            self.px[y][x] = tuple(
                int(c[i] * a / 255 + bg[i] * (255 - a) / 255) for i in range(3)
            )

    def circle_fill(self, cx, cy, r, c):
        for dy in range(-r, r + 1):
            dx = int(math.sqrt(max(0, r * r - dy * dy)))
            for x in range(cx - dx, cx + dx + 1):
                self.put(x, cy + dy, c)

    def circle_aa(self, cx, cy, r, c):
        for dy in range(-r - 1, r + 2):
            for dx in range(-r - 1, r + 2):
                d = math.sqrt(dx * dx + dy * dy)
                if d < r - 0.7:
                    continue
                if d <= r + 0.7:
                    a = int(255 * max(0, min(1, r + 0.7 - d)))
                    if a > 0:
                        self.put_aa(cx + dx, cy + dy, c, a)

    def oval_fill(self, cx, cy, rx, ry, c):
        for dy in range(-ry, ry + 1):
            if ry == 0:
                continue
            dx = int(rx * math.sqrt(max(0, 1 - (dy * dy) / (ry * ry))))
            for x in range(cx - dx, cx + dx + 1):
                self.put(x, cy + dy, c)

    def tri_fill(self, x1, y1, x2, y2, x3, y3, c):
        minx = max(0, min(x1, x2, x3))
        maxx = min(W - 1, max(x1, x2, x3))
        miny = max(0, min(y1, y2, y3))
        maxy = min(H - 1, max(y1, y2, y3))
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                if self._point_in_tri(x, y, x1, y1, x2, y2, x3, y3):
                    self.put(x, y, c)

    @staticmethod
    def _point_in_tri(px, py, x1, y1, x2, y2, x3, y3):
        d1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
        d2 = (x3 - x2) * (py - y2) - (y3 - y2) * (px - x2)
        d3 = (x1 - x3) * (py - y3) - (y1 - y3) * (px - x3)
        has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
        has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
        return not (has_neg and has_pos)

    def star4(self, cx, cy, outer_r, inner_r, c, angle=0):
        ra = math.radians(angle)
        pts = []
        for i in range(8):
            a = ra + i * math.pi / 4
            r = outer_r if i % 2 == 0 else inner_r
            pts.append((int(cx + r * math.cos(a)), int(cy + r * math.sin(a))))
        for i in range(4):
            self.tri_fill(cx, cy,
                          pts[2 * i][0], pts[2 * i][1],
                          pts[(2 * i + 1) % 8][0], pts[(2 * i + 1) % 8][1], c)
            self.tri_fill(cx, cy,
                          pts[(2 * i + 1) % 8][0], pts[(2 * i + 1) % 8][1],
                          pts[(2 * i + 2) % 8][0], pts[(2 * i + 2) % 8][1], c)

    # ── Floyd-Steinberg 抖动 → RGB565 ─────────────────

    def to_rgb565_dithered(self):
        """
        对画布逐像素执行 Floyd-Steinberg 误差扩散,
        将 24-bit RGB 量化到 16-bit RGB565。
        背景像素 BG 锁定不参与误差扩散。
        """
        # 深拷贝浮点数组用于误差累积
        r_chan = [[float(self.px[y][x][0]) for x in range(W)] for y in range(H)]
        g_chan = [[float(self.px[y][x][1]) for x in range(W)] for y in range(H)]
        b_chan = [[float(self.px[y][x][2]) for x in range(W)] for y in range(H)]

        result = [[0] * W for _ in range(H)]

        for y in range(H):
            for x in range(W):
                ro, go, bo = r_chan[y][x], g_chan[y][x], b_chan[y][x]

                # 背景像素: 跳过抖动, 直接锁定
                orig = self.px[y][x]
                if orig == BG:
                    result[y][x] = rgb888_to_565(BG[0], BG[1], BG[2])
                    continue

                # 裁剪到 [0, 255]
                ro, go, bo = max(0, min(255, ro)), max(0, min(255, go)), max(0, min(255, bo))

                # 最近 RGB565 值
                rq = round(ro * 31 / 255) * 255 / 31
                gq = round(go * 63 / 255) * 255 / 63
                bq = round(bo * 31 / 255) * 255 / 31

                r565 = int(round(ro * 31 / 255))
                g565 = int(round(go * 63 / 255))
                b565 = int(round(bo * 31 / 255))
                result[y][x] = (r565 << 11) | (g565 << 5) | b565

                # 量化误差
                er = ro - rq
                eg = go - gq
                eb = bo - bq

                # Floyd-Steinberg 扩散
                if x + 1 < W:
                    r_chan[y][x + 1] += er * 7 / 16
                    g_chan[y][x + 1] += eg * 7 / 16
                    b_chan[y][x + 1] += eb * 7 / 16
                if y + 1 < H:
                    if x - 1 >= 0:
                        r_chan[y + 1][x - 1] += er * 3 / 16
                        g_chan[y + 1][x - 1] += eg * 3 / 16
                        b_chan[y + 1][x - 1] += eb * 3 / 16
                    r_chan[y + 1][x] += er * 5 / 16
                    g_chan[y + 1][x] += eg * 5 / 16
                    b_chan[y + 1][x] += eb * 5 / 16
                    if x + 1 < W:
                        r_chan[y + 1][x + 1] += er * 1 / 16
                        g_chan[y + 1][x + 1] += eg * 1 / 16
                        b_chan[y + 1][x + 1] += eb * 1 / 16

        return result

    def to_c_array(self, dithered):
        """将 RGB565 整数矩阵转为 C 数组字符串"""
        lines = []
        for y in range(H):
            row = [rgb565_to_be_bytes(dithered[y][x]) for x in range(W)]
            lines.append("    " + ", ".join(row))
        return ",\n".join(lines)


# ═══════════════════════════════════════════════════════════════
#  基础狐狸 (头 + 身体) — 坐标按 130×130 缩放 (×1.625)
# ═══════════════════════════════════════════════════════════════

# 缩放因子: 130/80 = 1.625
S = 1.625


def draw_base_fox(c):
    """所有等级共用的基础狐: 圆头 + 椭圆身体 + 白肚皮 + 五官"""

    # === 身体 ===
    c.oval_fill(int(40 * S), int(63 * S), int(16 * S), int(13 * S), ORANGE)
    c.oval_fill(int(40 * S), int(65 * S), int(11 * S), int(9 * S), WHITE)
    # 小短手
    c.oval_fill(int(26 * S), int(60 * S), int(5 * S), int(7 * S), ORANGE)
    c.oval_fill(int(54 * S), int(60 * S), int(5 * S), int(7 * S), ORANGE)
    # 小短脚
    c.oval_fill(int(31 * S), int(74 * S), int(5 * S), int(4 * S), ORANGE)
    c.oval_fill(int(49 * S), int(74 * S), int(5 * S), int(4 * S), ORANGE)

    # === 头部 ===
    # 左耳
    c.tri_fill(int(20 * S), int(24 * S), int(12 * S), int(6 * S),
               int(30 * S), int(10 * S), ORANGE)
    c.tri_fill(int(20 * S), int(20 * S), int(15 * S), int(8 * S),
               int(28 * S), int(14 * S), PINK)
    # 右耳
    c.tri_fill(int(60 * S), int(24 * S), int(68 * S), int(6 * S),
               int(50 * S), int(10 * S), ORANGE)
    c.tri_fill(int(60 * S), int(20 * S), int(65 * S), int(8 * S),
               int(52 * S), int(14 * S), PINK)

    # 主脸
    c.circle_fill(int(40 * S), int(36 * S), int(22 * S), ORANGE)
    # 脸两侧蓬松毛
    c.oval_fill(int(23 * S), int(36 * S), int(6 * S), int(10 * S), ORANGE)
    c.oval_fill(int(57 * S), int(36 * S), int(6 * S), int(10 * S), ORANGE)

    # 白色吻部
    c.oval_fill(int(40 * S), int(43 * S), int(12 * S), int(10 * S), CREAM)
    c.circle_fill(int(40 * S), int(40 * S), int(10 * S), WHITE)

    # 眼睛
    c.circle_fill(int(30 * S), int(32 * S), int(6 * S), BLACK)
    c.circle_fill(int(50 * S), int(32 * S), int(6 * S), BLACK)
    # 高光
    c.circle_fill(int(28 * S), int(30 * S), int(2 * S), WHITE)
    c.circle_fill(int(48 * S), int(30 * S), int(2 * S), WHITE)
    c.circle_fill(int(31 * S), int(34 * S), int(1 * S), WHITE)
    c.circle_fill(int(51 * S), int(34 * S), int(1 * S), WHITE)

    # 鼻子
    c.oval_fill(int(40 * S), int(40 * S), int(3 * S), int(2 * S), BLACK)
    c.put(int(39 * S), int(39 * S), WHITE)

    # 嘴巴
    mouth_pts = [(38, 43), (39, 44), (40, 44), (41, 44), (42, 43)]
    for mx, my in mouth_pts:
        c.put(int(mx * S), int(my * S), DARKBROWN)

    # 眉毛
    c.oval_fill(int(30 * S), int(25 * S), int(4 * S), int(2 * S), ORANGE_D)
    c.oval_fill(int(50 * S), int(25 * S), int(4 * S), int(2 * S), ORANGE_D)

    # 腮红
    for dx in [-12, 12]:
        for r in range(1, 3):
            a = 60 - r * 20
            c.put_aa(int((40 + dx) * S), int(38 * S), PINK, a)


# ═══════════════════════════════════════════════════════════════
#  配件绘制
# ═══════════════════════════════════════════════════════════════

def draw_star(c):
    c.star4(int(40 * S), int(56 * S), int(8 * S), int(3 * S), GOLD)
    c.circle_fill(int(40 * S), int(56 * S), int(2 * S), (255, 240, 180))


def draw_cape(c):
    c.tri_fill(int(20 * S), int(48 * S), int(60 * S), int(48 * S),
               int(52 * S), int(78 * S), YELLOW)
    c.tri_fill(int(20 * S), int(48 * S), int(60 * S), int(48 * S),
               int(28 * S), int(78 * S), YELLOW)
    c.circle_fill(int(40 * S), int(46 * S), int(3 * S), RED)


def draw_book(c):
    c.tri_fill(int(26 * S), int(62 * S), int(40 * S), int(54 * S),
               int(40 * S), int(74 * S), RED)
    c.tri_fill(int(54 * S), int(62 * S), int(40 * S), int(54 * S),
               int(40 * S), int(74 * S), RED)
    c.tri_fill(int(28 * S), int(62 * S), int(40 * S), int(55 * S),
               int(40 * S), int(72 * S), WHITEBG)
    c.tri_fill(int(52 * S), int(62 * S), int(40 * S), int(55 * S),
               int(40 * S), int(72 * S), WHITEBG)
    c.put(int(40 * S), int(54 * S), DARKBROWN)
    c.put(int(40 * S), int(55 * S), DARKBROWN)


def draw_crown(c):
    for y in range(int(8 * S), int(14 * S)):
        for x in range(int(27 * S), int(54 * S)):
            c.put(x, y, GOLD)
    c.tri_fill(int(28 * S), int(8 * S), int(34 * S), 0, int(40 * S), int(8 * S), GOLD)
    c.tri_fill(int(34 * S), int(8 * S), int(40 * S), 0, int(46 * S), int(8 * S), GOLD)
    c.tri_fill(int(40 * S), int(8 * S), int(46 * S), 0, int(52 * S), int(8 * S), GOLD)
    c.circle_fill(int(40 * S), int(7 * S), int(2 * S), GEM_RED)
    c.circle_fill(int(39 * S), int(6 * S), int(1 * S), WHITE)


def draw_glow(c):
    for i in range(12):
        a = i * math.pi / 6
        for d in range(int(18 * S), int(36 * S), 2):
            x = int(40 * S + d * math.cos(a))
            y = int(42 * S + d * math.sin(a))
            alpha = max(20, int(120 * S / 1.625) - (d - int(18 * S)) * 6)
            c.put_aa(x, y, GOLD, alpha)


def draw_rainbow_glow(c):
    rainbow = [
        (255, 100, 255), (130, 160, 255), (100, 255, 200),
        (255, 255, 100), (255, 200, 80), (255, 120, 80),
    ]
    for i in range(12):
        a = i * math.pi / 6
        rc = rainbow[i % 6]
        for d in range(int(16 * S), int(38 * S), 2):
            x = int(40 * S + d * math.cos(a))
            y = int(42 * S + d * math.sin(a))
            alpha = max(25, int(140 * S / 1.625) - (d - int(16 * S)) * 7)
            c.put_aa(x, y, rc, alpha)
    c.oval_fill(int(18 * S), int(42 * S), int(4 * S), int(8 * S), WHITE)
    c.oval_fill(int(62 * S), int(42 * S), int(4 * S), int(8 * S), WHITE)
    for d in range(6):
        x = int(40 * S + 14 * S * math.cos(d * math.pi / 3))
        y = int(2 + 14 * S * math.sin(d * math.pi / 3))
        c.put_aa(x, y, GOLD, 180)


# ═══════════════════════════════════════════════════════════════
#  9 个等级
# ═══════════════════════════════════════════════════════════════

LEVEL_SPECS = [
    [],
    [draw_star],
    [draw_cape],
    [draw_book],
    [draw_cape, draw_star],
    [draw_cape, draw_book],
    [draw_crown],
    [draw_crown, draw_cape, draw_book, draw_glow],
    [draw_crown, draw_cape, draw_book, draw_rainbow_glow],
]


def make_level(accessories):
    c = Canvas()
    draw_base_fox(c)
    for acc in accessories:
        acc(c)
    return c


# ═══════════════════════════════════════════════════════════════
#  输出 fox_images.h
# ═══════════════════════════════════════════════════════════════

def generate_header():
    levels = []
    for i, accs in enumerate(LEVEL_SPECS):
        c = make_level(accs)
        dithered = c.to_rgb565_dithered()
        levels.append((i, c, dithered))
        print(f"  LV_{i}: drawn + FS-dithered ({len(accs)} accessories)")

    out = []
    out.append("""/**
 * fox_images.h — 9 阶段狐狸图像位图数据 (RGB565, Floyd-Steinberg 抖动) — 自动生成
 * =================================================================================
 * 尺寸: 130×130 px, RGB565, 33,800 bytes/张, 9 张共 ~297 KB
 * 工具: fox_pixel_gen.py (程序化几何绘制 + PIL Floyd-Steinberg 误差扩散)
 * 背景: 全部锁定为 (16,16,16) = #101010, 与 LVGL 屏幕底色完全融合
 *
 * 重要: 本文件由 fox_pixel_gen.py 自动生成, 请勿手动编辑。
 */
#ifndef FOX_IMAGES_H
#define FOX_IMAGES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FOX_IMG_W  130
#define FOX_IMG_H  130
#define FOX_IMG_SZ (FOX_IMG_W * FOX_IMG_H * 2)

""")

    level_descs = [
        "LV_0: 初始狐 — 橙毛圆脸+白吻+粉耳+大黑瞳+椭圆身体",
        "LV_1: 捧星狐 — +胸前四角金星",
        "LV_2: 披风狐 — +黄披风+红领结",
        "LV_3: 捧书狐 — +红封面打开的书",
        "LV_4: 星披狐 — 披风+星星组合",
        "LV_5: 书披狐 — 披风+书本组合",
        "LV_6: 王冠狐 — +金王冠+红宝石",
        "LV_7: 究极狐 — 王冠+披风+书本+周身金光",
        "LV_8: 终极狐 — 王冠+披风+书本+彩虹光芒+小翅膀+天使光环",
    ]

    for i, c, dithered in levels:
        out.append(f"/* ── {level_descs[i]} ── */")
        out.append(f"static const uint8_t img_fox_level_{i}_data[FOX_IMG_SZ] = {{")
        out.append(c.to_c_array(dithered))
        out.append("};\n")

    # lv_img_dsc_t 描述符
    out.append("/* ── LVGL 图像描述符 (Flash 直读) ── */")
    for i in range(9):
        out.append(f"""const lv_img_dsc_t img_fox_level_{i} = {{
    .header = {{ .cf = LV_IMG_CF_TRUE_COLOR, .always_zero = 0, .reserved = 0, .w = FOX_IMG_W, .h = FOX_IMG_H }},
    .data_size = FOX_IMG_SZ,
    .data      = img_fox_level_{i}_data
}};
""")

    # 索引表
    out.append("/* ── 快速索引表: O(1) 查图 ── */")
    out.append("const lv_img_dsc_t *const g_fox_img_table[9] = {")
    for i in range(9):
        out.append(f"    [FOX_LV_{i}] = &img_fox_level_{i},")
    out.append("};")

    out.append("\n#ifdef __cplusplus\n}\n#endif\n\n#endif /* FOX_IMAGES_H */")
    return "\n".join(out)


if __name__ == "__main__":
    print("fox_pixel_gen.py — 130×130 Floyd-Steinberg dithering")
    print("=" * 55)
    code = generate_header()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(script_dir, "fox_images.h")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(code)

    print(f"\n[OK] {out_path}")
    print(f"  Size: {len(code):,} bytes")
    print(f"  9 images x 130x130 RGB565 (Floyd-Steinberg dithered)")
    print(f"  Background locked to #101010 (16,16,16)")
