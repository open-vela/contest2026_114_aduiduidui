/**
 * status_panel.h — 状态面板 + 实时音量监控
 * =========================================
 * LVGL v8 / ESP32-S3 / 240x320 屏幕底部 80px 面板
 *
 * 布局 (240×80, 位于屏幕底部):
 *   ┌──────────────────────────────────────┐
 *   │  Lv.0  ◇◇◇◇◇◇◇◇◇◇◇◇  Mic ████░░  │
 *   │  ┌─ XP ──────────────┐  ┌─ Vol ────┐ │
 *   │  │■■■■■■■■░░░░░░░░░░░│  │■■■■■■■■■■│ │
 *   │  └────── 50/200 ─────┘  └─── 67% ───┘ │
 *   └──────────────────────────────────────┘
 *
 * 内存策略:
 *   - 所有 LVGL 对象创建时分配, 运行时仅更新值
 *   - 定时器回调中用 lv_label_set_text_fmt / lv_bar_set_value
 *   - 零 malloc 在热路径上, 无内存碎片
 */

#ifndef STATUS_PANEL_H
#define STATUS_PANEL_H

#include "lvgl.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 公共 API ── */

/**
 * 初始化状态面板。
 * - 在屏幕底部创建 240×80 面板
 * - 包含: 等级标签, XP 进度条, 音量条, 音量数值标签
 * - 启动 LVGL 定时器 (50ms) 自动刷新音量和经验显示
 * - 启动 FreeRTOS 任务持续读取 I2S 麦克风音量
 *
 * @param mic_chan  I2S 麦克风通道句柄 (由 main.c 传入)
 */
void lv_status_panel_init(i2s_chan_handle_t mic_chan);

/**
 * 手动刷新面板 (等级/XP 变化后调用)。
 * 也可由 LVGL 定时器自动调用。
 */
void status_panel_refresh(void);

/**
 * 获取最后一次读取的麦克风音量值 (0~100)。
 * 非阻塞, 直接返回静态变量。
 */
int  status_get_mic_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_PANEL_H */
