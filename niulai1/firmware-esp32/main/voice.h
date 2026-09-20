/**
 * voice.h — TTS 语音输出模块 (voice_task + I2S1 + MAX98357A)
 * ============================================================
 * 对外仅 3+2 个接口, 供 main.c / chat 链路调用。
 * 引脚定案: I2S1 复用 SD 卡槽三根线 BCLK=IO39 WS=IO38 DOUT=IO40 (见 TTS_DESIGN.md §2.3)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化: I2S1 + 消息队列 + voice_task (任务栈 8192, 优先级高于 cam/mic) */
void voice_init(void);

/* 非阻塞播报: 覆盖等待消息，并取消正在进行的旧 HTTP/解码/播放事务 */
void voice_speak_async(const char *text);

/* 打断: 清空队列；HTTP/解码在检查点取消，PCM 最迟在下一个 512-sample 块停止 */
void voice_interrupt(void);

/* 音量 0-100, 软件衰减 PCM (P3 播放链路启用) */
void voice_set_volume(uint8_t vol);

/* TTS 合成/下载/播放事务进行中返回 true；mic 可据此先打断、再录音 */
bool voice_is_playing(void);

/* 等待队列、合成与播放全部空闲；timeout_ms=0 表示只查询一次。 */
bool voice_wait_idle(uint32_t timeout_ms);

/* 开机提示音 (非阻塞): 本地合成 C5-E5-G5-C6 上行琶音 ~720ms,
 * 走 voice_task 队列播放, 不依赖网络, 兼做喇叭硬件自检 */
void voice_play_boot_chime(void);

/* P0 硬件哑测: 1kHz 正弦波 1s, 走 voice_task 队列, 永不阻塞调用方 */
void voice_test_tone(void);

#ifdef __cplusplus
}
#endif
