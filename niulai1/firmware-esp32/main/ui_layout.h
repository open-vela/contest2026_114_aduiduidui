/**
 * ui_layout.h — 智瞳伴读 LVGL 完整界面布局
 * =========================================
 * 240×320 屏幕, LVGL v8, 儿童圆润风格
 *
 * 从上到下:
 *   顶部: 日期 + 星期 + 时间
 *   音量: 麦克风图标 + 音量条 + 百分比
 *   中部: 狐狸精灵 (80×80 居中)
 *   等级: Lv.X + XP 进度条
 *   底部: FPS + "ZhiTong"
 *
 * 摄像头: 后台运行, 不显示到屏幕, 通过 capture_frame() 随时截取
 */

#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#include "lvgl.h"
#include "driver/i2s_std.h"
#include "esp_camera.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 公共 API ── */

/**
 * 初始化完整 UI 布局。
 * - 创建顶部日期/时间/星期标签
 * - 创建麦克风音量条 + 百分比标签
 * - 狐狸图像由 fox_companion 独立管理 (居中)
 * - 创建底部等级/XP/FPS 面板
 * - 启动 LVGL 定时器: 1s 更新日期, 100ms 更新音量+FPS
 * - 启动 FreeRTOS 后台任务持续读取 I2S 麦克风音量
 *
 * @param mic_chan  I2S 麦克风通道 (main.c 创建)
 */
void ui_layout_init(i2s_chan_handle_t mic_chan);

/**
 * 通知 UI 刷新 XP/等级显示。
 * (add_xp 内部已自动调用 update_fox_level,
 *  此函数用于从外部强制刷新 UI 文本)
 */
void ui_refresh_xp(void);

/**
 * 后台截取一帧摄像头画面 (RGB565, 240×240)。
 * 返回 esp_camera_fb_get() 的指针, 用完后调用 ui_frame_return() 归还。
 *
 * 用法:
 *   camera_fb_t *fb = ui_capture_frame();
 *   if (fb) {
 *       // ... 上传到云端 ...
 *       ui_frame_return(fb);
 *   }
 */
camera_fb_t *ui_capture_frame(void);
void         ui_frame_return(camera_fb_t *fb);

/**
 * 获取当前 FPS (每秒更新一次)。
 */
float ui_get_fps(void);

/**
 * 获取当前麦克风音量 (0~100)。
 */
int   ui_get_mic_volume(void);

/**
 * 获取当前扬声器音量 (0~100)。
 */
int   ui_get_spk_volume(void);

/**
 * 设置扬声器音量 (0~100), 自动刷新 UI 音量条。
 */
void  ui_set_spk_volume(int vol);

/**
 * 在屏幕上显示 IP 地址。
 */
void  ui_set_ip_text(const char *ip);

/**
 * 挂起/恢复麦克风 VU 后台读取。
 * ASR 录音期间必须挂起, 否则 mic_reader_task 与 do_asr 竞争
 * I2S 通道读锁 (i2s_common 的 binary 信号量), 录音丢数据。
 */
void  ui_set_mic_suspend(bool suspend);

/** 后台任务设置休息提示；实际 LVGL 更新由 200ms 定时器执行。 */
void  ui_set_reading_break(bool active);

#ifdef __cplusplus
}
#endif

#endif /* UI_LAYOUT_H */
