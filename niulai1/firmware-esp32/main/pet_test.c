/**
 * pet_test.c — 宠物逻辑测试实现 (纯 C, 非阻塞)
 * ==============================================
 * - 用 esp_timer_get_time() 做非阻塞时间判断
 * - 每 1000ms +10 XP, XP>=200 时升级 (0→7 循环)
 * - 仅等级真正改变时才调用 update_fox_image()
 * - 每次数值变动都调用 update_ui_numbers()
 */

#include "pet_test.h"
#include "xp_system.h"
#include "fox_companion.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "PetTest";

/* ── 宠物状态 (全局单例) ── */
static int pet_level     = 0;   /* 当前等级 0~7            */
static int pet_xp        = 0;   /* 当前经验 0~199          */
static int prev_level    = 0;   /* 上一次的等级 (用于判断是否换图) */
static int64_t last_ms   = 0;   /* 上一次 +10XP 的时间戳 (微秒)  */

/* ═══════════════════════════════════════════════════════════
 *  占位函数: 切换狐狸形态
 * ═══════════════════════════════════════════════════════════ */
static void update_fox_image(int level)
{
    /*
     * 调用 fox_companion 模块的 API 更换狐狸图片。
     * 内部会自动触发缩放弹跳动画。
     */
    update_fox_level(level);
    ESP_LOGI(TAG, "[Upgrade] Fox image → Lv.%d", level);
}

/* ═══════════════════════════════════════════════════════════
 *  占位函数: 刷新 UI 数字和进度条
 * ═══════════════════════════════════════════════════════════ */
static void update_ui_numbers(int level, int xp)
{
    /*
     * 同步 XP 系统内部状态, LVGL 的 200ms 定时器会自动
     * 读取并刷新 Lv.X 文字、XP XX/200 文字、进度条百分比。
     */
    xp_set_state(level, xp);
}

/* ═══════════════════════════════════════════════════════════
 *  主更新函数 (每帧调用, 非阻塞)
 * ═══════════════════════════════════════════════════════════ */
void pet_logic_test_update(void)
{
    /* ── 获取当前时间 (毫秒) ── */
    int64_t now_ms = esp_timer_get_time() / 1000;

    /* ── 首次调用时初始化时间戳 ── */
    if (last_ms == 0) {
        last_ms = now_ms;
        /* 从持久化的 XP 状态接续, 重启后不再从 0 开始 */
        pet_level  = xp_get_level();
        pet_xp     = xp_get_current();
        prev_level = pet_level;
        return;
    }

    /* ── 时间判定: 每隔 1000ms 增加 10 点经验 ── */
    if (now_ms - last_ms < 1000) {
        return;  /* 还没到 1 秒, 直接返回, 不阻塞 */
    }
    last_ms = now_ms;

    /* ── 经验值增加 ── */
    pet_xp += 10;

    /* ── 升级判定: XP >= 200 ── */
    int leveled = 0;  /* 标志位: 本次是否触发了升级 */

    if (pet_xp >= 200) {
        pet_level++;          /* 等级 +1              */
        pet_xp = 0;           /* 经验归零             */
        leveled = 1;          /* 标记发生了升级       */

        /* 等级循环: 超过 7 回到 0 */
        if (pet_level > 7) {
            pet_level = 0;
        }
    }

    /* ── 换图判定: 只有等级真正改变时才切换狐狸图片 ── */
    if (leveled) {
        ESP_LOGI(TAG, "[Upgrade] Level: %d -> %d", prev_level, pet_level);
        update_fox_image(pet_level);
        prev_level = pet_level;
    }

    /* ── UI 更新: 每次经验或等级变动都刷新数字和进度条 ── */
    update_ui_numbers(pet_level, pet_xp);
}
