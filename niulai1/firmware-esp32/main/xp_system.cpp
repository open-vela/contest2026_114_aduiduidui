/**
 * xp_system.cpp — 经验值与等级成长系统实现
 * ==========================================
 * ESP32-S3 / FreeRTOS / 全静态分配
 */

#include "xp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "XPSys";

/* ── 全局状态 (单例, 零分配) ── */
static int g_current_level = 0;
static int g_current_xp    = 0;
static portMUX_TYPE g_xp_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── NVS 持久化 (命名空间 zhitong, 键 xp_lv / xp_cur) ── */
#define NVS_NS     "zhitong"
#define NVS_KEY_LV "xp_lv"
#define NVS_KEY_XP "xp_cur"

/* 写 NVS (最佳努力: 失败仅告警, 不打扰主流程)。调用方在临界区外传入快照值。 */
static void xp_save_state(int lv, int xp) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open fail, xp not saved");
        return;
    }
    if (nvs_set_u32(h, NVS_KEY_LV, (uint32_t)lv) != ESP_OK ||
        nvs_set_u32(h, NVS_KEY_XP, (uint32_t)xp) != ESP_OK) {
        ESP_LOGW(TAG, "nvs set fail, xp not saved");
    }
    if (nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "nvs commit fail, xp not saved");
    nvs_close(h);
}

/* ── 公开实现 ── */

void xp_system_init(void) {
    /* 从 NVS 恢复上次等级/XP; 无记录或损坏时回退 0 级 */
    int lv = 0, xp = 0, restored = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t u_lv = 0, u_xp = 0;
        if (nvs_get_u32(h, NVS_KEY_LV, &u_lv) == ESP_OK &&
            nvs_get_u32(h, NVS_KEY_XP, &u_xp) == ESP_OK &&
            (int)u_lv >= 0 && (int)u_lv <= XP_LEVEL_MAX &&
            (int)u_xp >= 0 && (int)u_xp < XP_PER_LEVEL) {
            lv = (int)u_lv;
            xp = (int)u_xp;
            restored = 1;
        }
        nvs_close(h);
    }

    taskENTER_CRITICAL(&g_xp_lock);
    g_current_level = lv;
    g_current_xp    = xp;
    taskEXIT_CRITICAL(&g_xp_lock);

    if (restored)
        ESP_LOGI(TAG, "XP restored → Lv.%d, %d/%d XP", lv, xp, XP_PER_LEVEL);
    else
        ESP_LOGI(TAG, "XP system reset → Lv.0, 0/%d XP", XP_PER_LEVEL);
}

void add_xp(int xp) {
    if (xp <= 0) return;

    taskENTER_CRITICAL(&g_xp_lock);

    /* 满级不再累加 */
    if (g_current_level >= XP_LEVEL_MAX) {
        taskEXIT_CRITICAL(&g_xp_lock);
        return;
    }

    g_current_xp += xp;
    ESP_LOGI(TAG, "+%d XP → %d/%d (Lv.%d)", xp, g_current_xp, XP_PER_LEVEL, g_current_level);

    /* 检查升级 (支持一次跨越多个等级) */
    int leveled = 0;
    while (g_current_xp >= XP_PER_LEVEL && g_current_level < XP_LEVEL_MAX) {
        g_current_xp -= XP_PER_LEVEL;
        g_current_level++;
        leveled = 1;
        ESP_LOGI(TAG, ">> LEVEL UP! → Lv.%d <<", g_current_level);
    }

    /* 防止越界 */
    if (g_current_level >= XP_LEVEL_MAX) {
        g_current_level = XP_LEVEL_MAX;
        g_current_xp    = 0;
        ESP_LOGI(TAG, ">> MAX LEVEL reached! (Lv.%d) <<", XP_LEVEL_MAX);
    }

    int new_level = g_current_level;
    int save_xp   = g_current_xp;
    taskEXIT_CRITICAL(&g_xp_lock);

    /* 狐狸图像由 LVGL 定时器根据等级快照更新，避免后台任务直接调用 LVGL。 */
    if (leveled) {
        /* 只在升级时写 NVS: 级内 XP 进度断电丢失可接受, 避免高频擦写闪存 */
        xp_save_state(new_level, save_xp);
    }
}

int xp_get_level(void)   {
    taskENTER_CRITICAL(&g_xp_lock);
    int value = g_current_level;
    taskEXIT_CRITICAL(&g_xp_lock);
    return value;
}
int xp_get_current(void) {
    taskENTER_CRITICAL(&g_xp_lock);
    int value = g_current_xp;
    taskEXIT_CRITICAL(&g_xp_lock);
    return value;
}
int xp_get_max(void)     { return XP_PER_LEVEL; }
int xp_is_maxed(void)    { return xp_get_level() >= XP_LEVEL_MAX; }

void xp_set_level(int level) {
    if (level < 0) level = 0;
    if (level > XP_LEVEL_MAX) level = XP_LEVEL_MAX;
    taskENTER_CRITICAL(&g_xp_lock);
    g_current_level = level;
    g_current_xp    = 0;
    taskEXIT_CRITICAL(&g_xp_lock);
    xp_save_state(level, 0);
    ESP_LOGI(TAG, "Level set → %d", level);
}

void xp_set_state(int level, int xp) {
    if (level < 0) level = 0;
    if (level > XP_LEVEL_MAX) level = XP_LEVEL_MAX;
    if (xp < 0) xp = 0;
    if (xp >= XP_PER_LEVEL) xp = XP_PER_LEVEL - 1;

    taskENTER_CRITICAL(&g_xp_lock);
    int changed = (g_current_level != level);
    g_current_level = level;
    g_current_xp    = xp;
    taskEXIT_CRITICAL(&g_xp_lock);

    if (changed)
        xp_save_state(level, xp);   /* 仅等级变化时持久化 (PetTest 每秒刷 XP 值, 不能跟着写) */
}
