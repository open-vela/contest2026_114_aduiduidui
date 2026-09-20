/**
 * ui_layout.cpp — 智瞳伴读 UI (240x320 / LVGL v8 / 黑底金主题)
 *
 * 布局 (上→下):
 *   y=0:   日期 + 时间 (单调钟, 不跳)
 *   y=28:  Lv.3  XP 100/200
 *   y=52:  XP 进度条
 *   y=296: Mic 音量条 + %
 *
 * 狐狸: lv_fox_init 居中, 80x80
 * 时钟: 启动时获取一次, 不刷新 (用户要求静态)
 */

#include "ui_layout.h"
#include "xp_system.h"
#include "fox_companion.h"
#include "voice.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <time.h>

static const char *TAG = "UILayout";
#define SCR_W 240
#define SCR_H 320

/* ── 麦克风 ── */
#define MIC_SR 8000
#define MIC_MS  50
#define MIC_N  ((MIC_SR * MIC_MS) / 1000)
#define MIC_B  (MIC_N * 2)
static volatile int  g_vol = 0;
static volatile int  g_spk_vol = 50;   /* 扬声器音量 0~100, 默认50% */
static volatile bool g_mic_suspend = false;  /* ASR 录音期间挂起 VU 读取 */
static volatile bool g_reading_break = false;
static i2s_chan_handle_t g_mic = NULL;

/* LVGL */
static lv_obj_t *label_date = NULL;
static lv_obj_t *label_ip   = NULL;
static lv_obj_t *label_mood = NULL;
static lv_obj_t *label_break = NULL;
static lv_obj_t *label_lv   = NULL;
static lv_obj_t *bar_xp     = NULL;
static lv_obj_t *bar_vol    = NULL;
static lv_obj_t *label_vol  = NULL;
static lv_obj_t *bar_spk    = NULL;
static lv_obj_t *label_spk  = NULL;

/* ── 工具 ── */
static lv_obj_t *mk_label(lv_obj_t *p, int x, int y, const char *t, lv_color_t c) {
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, t);
    return l;
}
static lv_obj_t *mk_row(lv_obj_t *p, int y, int h) {
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_set_size(r, SCR_W, h); lv_obj_set_pos(r, 0, y);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}
static lv_obj_t *mk_bar(lv_obj_t *p, int x, int y, int w, lv_color_t ind) {
    lv_obj_t *b = lv_bar_create(p);
    lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, 12);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, ind, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 6, LV_PART_INDICATOR);
    lv_bar_set_range(b, 0, 100);
    return b;
}

/* ================================================================
 *  mic_reader_task
 * ================================================================ */
static void mic_reader_task(void *arg) {
    i2s_chan_handle_t ch = (i2s_chan_handle_t)arg;
    int16_t *buf = (int16_t *)malloc(MIC_B);
    if (!buf) { vTaskDelete(NULL); return; }
    while (1) {
        size_t rd = 0;
        if (g_mic_suspend) { vTaskDelay(1); continue; }   /* do_asr 独占 mic 期间让路 */
        if (i2s_channel_read(ch, buf, MIC_B, &rd, pdMS_TO_TICKS(100)) == ESP_OK && rd >= 200) {
            double sum = 0.0;
            int n = (int)(rd / 2);
            for (int i = 0; i < n; i++) {
                double s = (double)buf[i];
                sum += s * s;
            }
            int v = (int)(sqrt(sum / n) * 100.0 / 500.0);   /* gain x24 */
            if (v > 100) v = 100;
            if (v < 0) v = 0;
            g_vol = v;
        }
        vTaskDelay(1);
    }
    free(buf);
    vTaskDelete(NULL);
}

/* ================================================================
 *  200ms 定时器: 日期/时间/Lv/XP/音量 + NTP 首次同步
 * ================================================================ */
/* 1s: 日期+时间 */
static void timer_1s_cb(lv_timer_t *t) {
    (void)t;
    if (!label_date) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    lv_label_set_text_fmt(label_date, "%04d-%02d-%02d %s  %02d:%02d:%02d",
        tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
        wd[tm->tm_wday], tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* 200ms: Lv + XP + Mic */
static void timer_200ms_cb(lv_timer_t *t) {
    (void)t;
    int lv = xp_get_level();
    int xp = xp_get_current();
    int mx = xp_get_max();
    const FoxState *fox = fox_get_state();
    if (fox && (int)fox->level != lv) update_fox_level(lv);
    if (label_lv) {
        if (xp_is_maxed())
            lv_label_set_text_fmt(label_lv, "Lv.MAX  XP %d/%d", xp, mx);
        else
            lv_label_set_text_fmt(label_lv, "Lv.%d  XP %d/%d", lv, xp, mx);
    }
    if (bar_xp) lv_bar_set_value(bar_xp, (int32_t)(xp * 100 / mx), LV_ANIM_OFF);
    if (bar_vol) lv_bar_set_value(bar_vol, g_vol, LV_ANIM_OFF);
    if (label_vol) lv_label_set_text_fmt(label_vol, "%d%%", g_vol);
    if (bar_spk) lv_bar_set_value(bar_spk, g_spk_vol, LV_ANIM_OFF);
    if (label_spk) lv_label_set_text_fmt(label_spk, "%d%%", g_spk_vol);
    if (label_mood) lv_label_set_text_fmt(label_mood, "Mood: %s", fox_get_emotion_name());
    if (label_break) {
        if (g_reading_break) {
            lv_label_set_text(label_break, "REST: look far + blink");
            lv_obj_clear_flag(label_break, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label_break, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ================================================================
 *  ui_layout_init
 * ================================================================ */
void ui_layout_init(i2s_chan_handle_t mic_chan) {
    g_mic = mic_chan;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);

    /* R1: 日期+时间 (仅初始化一次, 不刷新) */
    lv_obj_t *r1 = mk_row(scr, 0, 26);
    label_date = lv_label_create(r1);
    lv_obj_set_pos(label_date, 2, 5);
    lv_obj_set_style_text_color(label_date, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(label_date, &lv_font_montserrat_14, 0);


    {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        lv_label_set_text_fmt(label_date, "%04d-%02d-%02d %s  %02d:%02d:%02d",
            tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
            wd[tm->tm_wday], tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    /* R2: Lv + XP 文本 */
    lv_obj_t *r2 = mk_row(scr, 28, 22);
    label_lv = mk_label(r2, 2, 3, "Lv.0  XP 0/200", lv_color_hex(0xFFD700));

    /* R3: XP 进度条 */
    lv_obj_t *r3 = mk_row(scr, 52, 16);
    bar_xp = mk_bar(r3, 2, 2, 236, lv_color_hex(0xFFD700));
    lv_bar_set_value(bar_xp, 50, LV_ANIM_OFF);

    /* IP 标签 (XP条下方) */
    label_ip = lv_label_create(scr);
    lv_obj_set_pos(label_ip, 2, 70);
    lv_obj_set_style_text_color(label_ip, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label_ip, &lv_font_montserrat_14, 0);
    lv_label_set_text(label_ip, "WiFi...");

    label_mood = mk_label(scr, 2, 90, "Mood: idle", lv_color_hex(0xAAAAAA));
    label_break = mk_label(scr, 2, 108, "REST: look far + blink", lv_color_hex(0xFF8C00));
    lv_obj_add_flag(label_break, LV_OBJ_FLAG_HIDDEN);

    /* GOT_IP 事件 (t≈2.4s) 早于本函数 (t≈9.6s), 事件回调里 label 还是 NULL 会被丢弃
     * → 这里主动从 esp_netif 拉一次当前 IP, 兜底首次启动; 之后的重连走 wcb 正常更新 */
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
        char buf[32];
        snprintf(buf, sizeof(buf), "http://" IPSTR "/", IP2STR(&ip.ip));
        lv_label_set_text(label_ip, buf);
    }

    /* R4: Mic 音量条 */
    lv_obj_t *r4 = mk_row(scr, 212, 14);
    mk_label(r4, 2, 0, "Mic", lv_color_hex(0xFFD700));
    bar_vol = mk_bar(r4, 36, 1, 100, lv_color_hex(0x00FF00));
    label_vol = mk_label(r4, 142, 0, "0%", lv_color_hex(0xFFD700));

    /* R5: Speaker 音量条 */
    lv_obj_t *r5 = mk_row(scr, 226, 14);
    mk_label(r5, 2, 0, "Spk", lv_color_hex(0xFFD700));
    bar_spk = mk_bar(r5, 36, 1, 100, lv_color_hex(0x00BFFF));
    lv_bar_set_value(bar_spk, 70, LV_ANIM_OFF);
    label_spk = mk_label(r5, 142, 0, "50%", lv_color_hex(0xFFD700));

    /* 狐狸居中 */
    const FoxState *fs = fox_get_state();
    if (fs && fs->img_obj) lv_obj_align(fs->img_obj, LV_ALIGN_CENTER, 0, -12);

    /* Mic Reader: 实时监听音量 */
    i2s_channel_enable(mic_chan);
    xTaskCreate(mic_reader_task, "mic_rdr", 4096, (void *)mic_chan, 1, NULL);

    /* 定时器: 1s 日期时间, 200ms Lv/XP/音量 */
    lv_timer_create(timer_1s_cb,   1000, NULL);
    lv_timer_create(timer_200ms_cb, 200, NULL);

    ESP_LOGI(TAG, "UI ready");
}

/* ── 公开 ── */
void ui_refresh_xp(void)        { timer_200ms_cb(NULL); }
camera_fb_t *ui_capture_frame(void)  { return esp_camera_fb_get(); }
void ui_frame_return(camera_fb_t *fb) { if (fb) esp_camera_fb_return(fb); }
float ui_get_fps(void)          { return 0.0f; }
int   ui_get_mic_volume(void)   { return g_vol; }
int   ui_get_spk_volume(void)   { return g_spk_vol; }
void  ui_set_spk_volume(int v)  {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_spk_vol = v;
    voice_set_volume((uint8_t)v);
}
void  ui_set_ip_text(const char *ip) { if (label_ip && ip) lv_label_set_text(label_ip, ip); }
void  ui_set_mic_suspend(bool suspend) { g_mic_suspend = suspend; }
void  ui_set_reading_break(bool active) { g_reading_break = active; }
