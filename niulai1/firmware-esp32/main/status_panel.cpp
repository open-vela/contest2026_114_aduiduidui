/**
 * status_panel.cpp — 状态面板 + 实时音量监控
 * ============================================
 * ESP32-S3 / LVGL v8 / 绝对定位 (零依赖 flex/recoolor)
 *
 * 布局 (240x80, 屏幕底部):
 *   ┌─────────────────────────────────────────┐
 *   │  Lv.0       经验: 50/200      Mic: 67%  │
 *   │  [████████░░░░░░░░░░░░░]  [████████░░]  │
 *   └─────────────────────────────────────────┘
 */

#include "status_panel.h"
#include "xp_system.h"
#include "esp_log.h"

static const char *TAG = "StPanel";

/* ── 音量数据 ── */
static volatile int g_mic_volume = 0;

/* LVGL 对象 */
static lv_obj_t *label_level = NULL;
static lv_obj_t *label_xp    = NULL;
static lv_obj_t *bar_xp      = NULL;
static lv_obj_t *label_vol   = NULL;
static lv_obj_t *bar_vol     = NULL;

/* ── LVGL 定时器: 100ms 刷新 ── */
static void panel_timer_cb(lv_timer_t *timer) {
    (void)timer;
    int lv = xp_get_level();
    int xp = xp_get_current();
    int mx = xp_get_max();

    if (label_level) {
        if (xp_is_maxed())
            lv_label_set_text_fmt(label_level, "Lv.MAX");
        else
            lv_label_set_text_fmt(label_level, "Lv.%d", lv);
    }

    if (label_xp)
        lv_label_set_text_fmt(label_xp, "XP: %d/%d", xp, mx);

    if (bar_xp)
        lv_bar_set_value(bar_xp, xp * 100 / mx, LV_ANIM_OFF);

    if (bar_vol)
        lv_bar_set_value(bar_vol, g_mic_volume, LV_ANIM_OFF);

    if (label_vol)
        lv_label_set_text_fmt(label_vol, "%d%%", g_mic_volume);
}

/* ── 公开: 初始化 ── */
void lv_status_panel_init(i2s_chan_handle_t mic_chan) {
    (void)mic_chan;  /* 保留接口, 后续接真实音量 */

    /* ── 容器背景 ── */
    lv_obj_t *bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bg, 240, 80);
    lv_obj_align(bg, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_90, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 4, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 等级标签 (左上) ── */
    label_level = lv_label_create(bg);
    lv_obj_set_pos(label_level, 6, 4);
    lv_obj_set_style_text_color(label_level, lv_color_hex(0xFFB800), 0);
    lv_obj_set_style_text_font(label_level, &lv_font_montserrat_14, 0);
    lv_label_set_text(label_level, "Lv.0");

    /* ── XP 文本 (中上) ── */
    label_xp = lv_label_create(bg);
    lv_obj_set_pos(label_xp, 6, 24);
    lv_obj_set_style_text_color(label_xp, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_xp, &lv_font_montserrat_14, 0);
    lv_label_set_text(label_xp, "XP: 0/200");

    /* ── XP 进度条 ── */
    bar_xp = lv_bar_create(bg);
    lv_obj_set_pos(bar_xp, 6, 44);
    lv_obj_set_size(bar_xp, 130, 12);
    lv_obj_set_style_bg_color(bar_xp, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_xp, lv_color_hex(0xFFB800), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_xp, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_xp, 4, LV_PART_INDICATOR);
    lv_bar_set_range(bar_xp, 0, 100);
    lv_bar_set_value(bar_xp, 0, LV_ANIM_OFF);

    /* ── 音量文本 (右上) ── */
    label_vol = lv_label_create(bg);
    lv_obj_set_pos(label_vol, 160, 4);
    lv_obj_set_style_text_color(label_vol, lv_color_hex(0x07E0), 0);
    lv_obj_set_style_text_font(label_vol, &lv_font_montserrat_14, 0);
    lv_label_set_text(label_vol, "0%");

    /* ── 音量条 ── */
    bar_vol = lv_bar_create(bg);
    lv_obj_set_pos(bar_vol, 160, 24);
    lv_obj_set_size(bar_vol, 70, 12);
    lv_obj_set_style_bg_color(bar_vol, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vol, lv_color_hex(0x07E0), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_vol, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_vol, 4, LV_PART_INDICATOR);
    lv_bar_set_range(bar_vol, 0, 100);
    lv_bar_set_value(bar_vol, 0, LV_ANIM_OFF);

    /* ── Mic 标签 ── */
    lv_obj_t *mic_label = lv_label_create(bg);
    lv_obj_set_pos(mic_label, 160, 44);
    lv_obj_set_style_text_color(mic_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(mic_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(mic_label, "Mic");

    /* ── 启动刷新定时器 (100ms → 10fps, 省 CPU) ── */
    lv_timer_create(panel_timer_cb, 100, NULL);

    ESP_LOGI(TAG, "Status panel ready");
}

void status_panel_refresh(void) {
    panel_timer_cb(NULL);
}

int status_get_mic_volume(void) {
    return g_mic_volume;
}
