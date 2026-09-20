#ifndef CHAT_CLIENT_H
#define CHAT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 用 MiMo Chat 将一条 ASR 文本转换为儿童友好的简短回答。
 * 返回值为 true 时 reply 已写入 NUL 结尾文本；不会保存长期对话历史，
 * 这样可避免 ESP32 上下文无限增长。调用线程会阻塞至网络超时或完成。
 */
bool chat_request_reply(const char *question, char *reply, size_t reply_size);

#ifdef __cplusplus
}
#endif

#endif
