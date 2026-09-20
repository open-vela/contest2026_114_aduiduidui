#include "chat_client.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "secrets.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "Chat";
extern esp_err_t esp_crt_bundle_attach(void *conf);

#define CHAT_URL MIMO_API_URL
#define CHAT_RESPONSE_MAX (16U * 1024U)
#define CHAT_REPLY_MAX 768U

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool append_utf8(char *dst, size_t cap, size_t *used, uint32_t cp)
{
    uint8_t out[4];
    size_t n;
    if (cp <= 0x7f) { out[0] = (uint8_t)cp; n = 1; }
    else if (cp <= 0x7ff) {
        out[0] = 0xc0 | (uint8_t)(cp >> 6); out[1] = 0x80 | (uint8_t)(cp & 0x3f); n = 2;
    } else if (cp <= 0xffff) {
        if (cp >= 0xd800 && cp <= 0xdfff) return false;
        out[0] = 0xe0 | (uint8_t)(cp >> 12); out[1] = 0x80 | (uint8_t)((cp >> 6) & 0x3f);
        out[2] = 0x80 | (uint8_t)(cp & 0x3f); n = 3;
    } else if (cp <= 0x10ffff) {
        out[0] = 0xf0 | (uint8_t)(cp >> 18); out[1] = 0x80 | (uint8_t)((cp >> 12) & 0x3f);
        out[2] = 0x80 | (uint8_t)((cp >> 6) & 0x3f); out[3] = 0x80 | (uint8_t)(cp & 0x3f); n = 4;
    } else return false;
    if (*used > cap - 1 || n > cap - 1 - *used) return false;
    memcpy(dst + *used, out, n); *used += n; return true;
}

static bool json_string(const char *json, const char *key, char *dst, size_t cap)
{
    if (!json || !key || !dst || cap == 0) return false;
    char pattern[40];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) return false;
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return false;

    size_t used = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c != '\\') {
            if (used + 1 >= cap) return false;
            dst[used++] = (char)c;
            continue;
        }
        char esc = *p++;
        if (!esc) return false;
        uint32_t cp = 0;
        if (esc == 'u') {
            if (!p[0] || !p[1] || !p[2] || !p[3]) return false;
            for (int i = 0; i < 4; i++) {
                int d = hex_value(p[i]); if (d < 0) return false;
                cp = (cp << 4) | (uint32_t)d;
            }
            p += 4;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                uint32_t low = 0;
                if (p[0] != '\\' || p[1] != 'u') return false;
                for (int i = 0; i < 4; i++) {
                    int d = hex_value(p[2 + i]); if (d < 0) return false;
                    low = (low << 4) | (uint32_t)d;
                }
                if (low < 0xdc00 || low > 0xdfff) return false;
                p += 6; cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
        } else {
            switch (esc) {
                case '"': cp = '"'; break; case '\\': cp = '\\'; break; case '/': cp = '/'; break;
                case 'b': cp = '\b'; break; case 'f': cp = '\f'; break; case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break; case 't': cp = '\t'; break; default: return false;
            }
        }
        if (!append_utf8(dst, cap, &used, cp)) return false;
    }
    if (*p != '"' || used == 0) return false;
    dst[used] = 0;
    return true;
}

static char *json_escape_psram(const char *src)
{
    size_t n = strlen(src);
    if (n > (SIZE_MAX - 1) / 6) return NULL;
    char *dst = heap_caps_malloc(n * 6 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) return NULL;
    static const char hex[] = "0123456789abcdef";
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[out++] = '\\'; dst[out++] = (char)c; }
        else if (c == '\n' || c == '\r' || c == '\t') {
            dst[out++] = '\\'; dst[out++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        } else if (c < 0x20) {
            dst[out++] = '\\'; dst[out++] = 'u'; dst[out++] = '0'; dst[out++] = '0';
            dst[out++] = hex[c >> 4]; dst[out++] = hex[c & 0xf];
        } else dst[out++] = (char)c;
    }
    dst[out] = 0;
    return dst;
}

bool chat_request_reply(const char *question, char *reply, size_t reply_size)
{
    if (!question || !question[0] || !reply || reply_size < 2 || MIMO_KEY[0] == 0) return false;
    reply[0] = 0;
    char *escaped = json_escape_psram(question);
    char *request = NULL;
    char *body = NULL;
    char *auth = NULL;
    esp_http_client_handle_t client = NULL;
    bool opened = false;
    bool ok = false;
    if (!escaped) goto cleanup;
    static const char *fmt =
        "{\"model\":\"mimo-v2.5\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是儿童绘本小助手。用简短、准确、友善的中文回答，不编造看不清的内容。\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":256,\"temperature\":0.4}";
    int request_len = snprintf(NULL, 0, fmt, escaped);
    if (request_len <= 0) goto cleanup;
    request = heap_caps_malloc((size_t)request_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) goto cleanup;
    snprintf(request, (size_t)request_len + 1, fmt, escaped);
    size_t auth_len = strlen(MIMO_KEY) + sizeof("Bearer ");
    auth = malloc(auth_len);
    if (!auth) goto cleanup;
    snprintf(auth, auth_len, "Bearer %s", MIMO_KEY);

    esp_http_client_config_t cfg = {
        .url = CHAT_URL, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
        .buffer_size = 2048, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&cfg);
    if (!client) goto cleanup;
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_open(client, request_len);
    if (err != ESP_OK) goto cleanup;
    opened = true;
    int sent = 0;
    while (sent < request_len) {
        int n = esp_http_client_write(client, request + sent, request_len - sent);
        if (n <= 0) goto cleanup;
        sent += n;
    }
    heap_caps_free(request); request = NULL;
    heap_caps_free(escaped); escaped = NULL;
    if (esp_http_client_fetch_headers(client) < 0) goto cleanup;
    int status = esp_http_client_get_status_code(client);
    int64_t content_len = esp_http_client_get_content_length(client);
    if (content_len > CHAT_RESPONSE_MAX) goto cleanup;
    body = heap_caps_malloc(CHAT_RESPONSE_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) goto cleanup;
    size_t total = 0;
    while (total < CHAT_RESPONSE_MAX) {
        if (content_len >= 0 && (int64_t)total == content_len) break;
        int got = esp_http_client_read(client, body + total, CHAT_RESPONSE_MAX - total);
        if (got < 0) goto cleanup;
        if (got == 0) break;
        total += (size_t)got;
    }
    body[total] = 0;
    if (!esp_http_client_is_complete_data_received(client) || status != 200) {
        ESP_LOGW(TAG, "HTTP %d or truncated response (%u bytes): %.300s",
                 status, (unsigned)total, total ? body : "<empty>");
        goto cleanup;
    }
    ok = json_string(body, "content", reply, reply_size);
    if (!ok) ESP_LOGW(TAG, "chat response has no usable content: %.300s", total ? body : "<empty>");

cleanup:
    if (opened && client) (void)esp_http_client_close(client);
    if (client) esp_http_client_cleanup(client);
    free(auth); heap_caps_free(escaped); heap_caps_free(request); heap_caps_free(body);
    return ok;
}
