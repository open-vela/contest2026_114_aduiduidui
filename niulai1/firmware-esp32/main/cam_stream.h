/**
 * cam_stream.h — WebSocket 低延迟视频推流
 * =========================================
 * 320x240 JPEG, quality 50, ~10fps, WebSocket 二进制帧
 */

#ifndef CAM_STREAM_H
#define CAM_STREAM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动摄像头 WebSocket 推流服务。
 * - 初始化摄像头 (QSXGA→QVGA, JPEG quality 50)
 * - 启动 HTTP 服务器 (端口 80)
 * - 注册 WebSocket 端点 /ws
 * - 创建 FreeRTOS 任务持续抓图广播
 */
void cam_stream_init(void);

/**
 * 暂停/恢复视频推流。
 * pause=true 时抓图任务休眠(不抓帧不广播), 客户端 TCP 连接保留。
 * 用途: ASR 录音上传(~64KB) / TTS 下载期间独占 WiFi 上行, 避免
 * 与 ~44KB/s 视频流竞争导致上传停滞。
 */
void cam_stream_pause(bool pause);

#ifdef __cplusplus
}
#endif

#endif /* CAM_STREAM_H */
