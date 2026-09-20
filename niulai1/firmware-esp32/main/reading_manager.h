#ifndef READING_MANAGER_H
#define READING_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 启动逐句朗读与 25 分钟阅读监测任务。 */
bool reading_manager_init(void);

/** 提交 OCR 文本；新页面会取消旧页面并覆盖待处理文本。 */
bool reading_start_text(const char *text);

/** 取消当前绘本朗读并清空待处理页面。 */
void reading_stop(void);

/** 立即拍摄当前画面并执行云端 OCR，再开始逐句朗读。 */
bool reading_capture_and_start(void);

#ifdef __cplusplus
}
#endif

#endif
