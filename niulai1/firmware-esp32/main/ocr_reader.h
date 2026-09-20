#ifndef OCR_READER_H
#define OCR_READER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 将 JPEG 帧送至 MiMo 视觉模型，提取绘本文字并按阅读顺序返回。 */
bool ocr_request_text(const camera_fb_t *fb, char *text, size_t text_size);

#ifdef __cplusplus
}
#endif

#endif
