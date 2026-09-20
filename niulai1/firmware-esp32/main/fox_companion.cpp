/**
 * fox_companion.cpp — 小狐狸陪伴精灵实现
 * ==========================================
 * ESP32-S3-EYE / LVGL v8 / RGB565 / 240×240
 * 内存策略: 全静态分配, 零堆碎片, Flash 直读图像
 */

#include "fox_companion.h"
#include "fox_images.h"
#include "xp_system.h"
#include "esp_log.h"

/* ---- 常量 ---- */
static const char *TAG = "FoxComp";

/* 8 个等级的中文名称 (存 Flash) */
static const char *k_level_names[8] = {
    "LV_0 初始狐",
    "LV_1 捧星狐",
    "LV_2 披风狐",
    "LV_3 捧书狐",
    "LV_4 星披狐",
    "LV_5 书披狐",
    "LV_6 王冠狐",
    "LV_7 究极狐"
};

static const char *k_emotion_names[FOX_EMOTE_MAX] = {
    "idle", "happy", "surprised", "reading", "sleepy", "proud", "curious", "encourage"
};

/* ---- 全局单例: 全静态分配, 零 malloc ---- */
static FoxState g_fox = {
    .level          = FOX_LV_0,
    .emotion        = FOX_EMOTE_IDLE,
    .img_obj        = NULL,
    .anim_busy      = false,
    .last_update_ms = 0,
    .level_name     = k_level_names[0]
};

/* ================================================================
 *  动画回调: 缩放反弹第二阶段 (1.15 → 1.0, 缓入)
 * ================================================================ */
static void _anim_bounce_back_cb(void *var, int32_t v) {
    lv_img_set_zoom((lv_obj_t *)var, (uint16_t)v);
}

static void _anim_bounce_back_done(lv_anim_t *a) {
    g_fox.anim_busy = false;
}

/* ================================================================
 *  动画回调: 缩放反弹第一阶段 (0.5 → 1.15, 缓出+过冲)
 *  完成后链式触发第二阶段
 * ================================================================ */
static void _anim_bounce_overshoot_cb(void *var, int32_t v) {
    lv_img_set_zoom((lv_obj_t *)var, (uint16_t)v);
}

static void _anim_bounce_overshoot_done(lv_anim_t *a) {
    /* 第一阶段结束 → 触发第二阶段: 1.15 → 1.0 回弹 */
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, a->var);
    lv_anim_set_exec_cb(&a2, _anim_bounce_back_cb);
    lv_anim_set_values(&a2, LV_IMG_ZOOM_NONE + (LV_IMG_ZOOM_NONE / 7), LV_IMG_ZOOM_NONE);
    lv_anim_set_time(&a2, 150);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a2, _anim_bounce_back_done);
    lv_anim_start(&a2);
}

/* ================================================================
 *  缩放弹跳动画: 0.35 → 1.15 (150ms, overshoot) → 1.0 (100ms, ease-in)
 *  总共 ~250ms, 视觉上"弹出来→弹回去"
 * ================================================================ */
static void _fox_anim_bounce(lv_obj_t *obj) {
    if (!obj) return;

    /* 先取消未完成的旧动画, 防止冲突 */
    lv_anim_del(obj, _anim_bounce_overshoot_cb);
    lv_anim_del(obj, _anim_bounce_back_cb);

    g_fox.anim_busy = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, _anim_bounce_overshoot_cb);
    /* 0.35x → 1.15x zoom (256 = 1.0x) */
    lv_anim_set_values(&a,
        (int32_t)(LV_IMG_ZOOM_NONE * 0.35f),
        (int32_t)(LV_IMG_ZOOM_NONE * 1.15f));
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_ready_cb(&a, _anim_bounce_overshoot_done);
    lv_anim_start(&a);
}

/* ================================================================
 *  lv_fox_init — 初始化狐狸精灵
 * ================================================================ */
void lv_fox_init(lv_obj_t *parent) {
    if (parent == NULL) {
        parent = lv_scr_act();
    }

    /* 创建图像对象 */
    g_fox.img_obj = lv_img_create(parent);

    /* 设置初始图像 (LV_0) */
    lv_img_set_src(g_fox.img_obj, &img_fox_level_0);

    /* 居中: 240×240 屏幕, 80×80 狐图 → 位置 (80, 80) */
    lv_obj_center(g_fox.img_obj);

    /* 锚点居中, 确保缩放围绕图像中心 */
    lv_obj_set_align(g_fox.img_obj, LV_ALIGN_CENTER);

    /* 重置状态 */
    g_fox.level          = FOX_LV_0;
    g_fox.emotion        = FOX_EMOTE_IDLE;
    g_fox.level_name     = k_level_names[0];
    g_fox.anim_busy      = false;
    g_fox.last_update_ms = lv_tick_get();

    /* 图像等级与 XP 系统同步: XP 从 NVS 恢复后可能不是 0 级 */
    int start_level = xp_get_level();
    if (start_level > 0) {
        update_fox_level(start_level);
    }
    ESP_LOGI(TAG, "Fox companion initialized @ level %d", start_level);
}

/* ================================================================
 *  update_fox_level — 切换等级 + 动画
 * ================================================================ */
void update_fox_level(int level) {
    /* Clamp */
    if (level < 0) level = 0;
    if (level > 7) level = 7;

    /* 同等级不重复触发 */
    if ((int)g_fox.level == level) return;

    /* 动画进行中: 直接换图 (跳帧), 但不触发新动画 */
    g_fox.level      = (FoxLevel)level;
    g_fox.level_name = k_level_names[level];

    if (g_fox.img_obj) {
        /* 更新图像源 → 立即生效 */
        lv_img_set_src(g_fox.img_obj, g_fox_img_table[level]);

        /* 仅在无动画时触发弹跳 */
        if (!g_fox.anim_busy) {
            _fox_anim_bounce(g_fox.img_obj);
        }
    }

    g_fox.last_update_ms = lv_tick_get();
    ESP_LOGI(TAG, "Fox level → %d (%s)", level, k_level_names[level]);
}

/* ================================================================
 *  fox_set_emotion — 切换情绪 (仅状态标记)
 * ================================================================ */
void fox_set_emotion(FoxEmotion emotion) {
    if (emotion >= FOX_EMOTE_MAX) emotion = FOX_EMOTE_IDLE;
    g_fox.emotion = emotion;
}

void fox_handle_event(FoxEvent event) {
    FoxEmotion next = FOX_EMOTE_IDLE;
    switch (event) {
        case FOX_EVENT_READING_START: next = FOX_EMOTE_READING; break;
        case FOX_EVENT_OCR_SUCCESS: next = FOX_EMOTE_HAPPY; break;
        case FOX_EVENT_QUESTION: next = FOX_EMOTE_CURIOUS; break;
        case FOX_EVENT_ANSWER_SUCCESS: next = FOX_EMOTE_HAPPY; break;
        case FOX_EVENT_ANSWER_ERROR: next = FOX_EMOTE_ENCOURAGE; break;
        case FOX_EVENT_BREAK: next = FOX_EMOTE_SLEEPY; break;
        case FOX_EVENT_BREAK_DONE: next = FOX_EMOTE_ENCOURAGE; break;
        case FOX_EVENT_IDLE:
        default: next = FOX_EMOTE_IDLE; break;
    }
    fox_set_emotion(next);
}

/* ================================================================
 *  fox_get_level_name — 读取当前等级名称
 * ================================================================ */
const char *fox_get_level_name(void) {
    return g_fox.level_name;
}

const char *fox_get_emotion_name(void) {
    int index = (int)g_fox.emotion;
    if (index < 0 || index >= FOX_EMOTE_MAX) index = FOX_EMOTE_IDLE;
    return k_emotion_names[index];
}

/* ================================================================
 *  fox_get_state — 获取全局只读状态指针
 * ================================================================ */
const FoxState *fox_get_state(void) {
    return &g_fox;
}

/* ================================================================
 *  fox_play_bounce — 手动触发弹跳动画
 * ================================================================ */
void fox_play_bounce(void) {
    if (g_fox.img_obj && !g_fox.anim_busy) {
        _fox_anim_bounce(g_fox.img_obj);
    }
}
