#include "ocr_reader.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "secrets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "OCR";
extern esp_err_t esp_crt_bundle_attach(void *conf);

#define OCR_URL MIMO_API_URL
#define OCR_RESPONSE_MAX (16U * 1024U)
#define OCR_IMAGE_MAX (180U * 1024U)

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool append_utf8(char *dst, size_t cap, size_t *used, uint32_t cp)
{
    uint8_t out[4]; size_t n;
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

static bool json_string(const char *json, char *dst, size_t cap)
{
    const char *p = strstr(json, "\"content\"");
    if (!p || cap < 2) return false;
    p += strlen("\"content\"");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        uint32_t cp = c;
        if (c != '\\') {
            /* 非转义内容通常已经是 UTF-8 原字节，不能把每个字节当作
             * Unicode 码点再次编码，否则中文会被破坏。 */
            if (n + 1 >= cap) return false;
            dst[n++] = (char)c;
            continue;
        }
        if (c == '\\') {
            char e = *p++;
            if (!e) return false;
            if (e == 'u') {
                if (!p[0] || !p[1] || !p[2] || !p[3]) return false;
                cp = 0;
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
                switch (e) {
                    case 'n': cp = '\n'; break; case 'r': cp = '\r'; break;
                    case 't': cp = '\t'; break; case '"': cp = '"'; break;
                    case '\\': cp = '\\'; break; case '/': cp = '/'; break;
                    case 'b': cp = '\b'; break; case 'f': cp = '\f'; break;
                    default: return false;
                }
            }
        }
        if (!append_utf8(dst, cap, &n, cp)) return false;
    }
    if (*p != '"' || n == 0) return false;
    dst[n] = 0;
    return true;
}

bool ocr_request_text(const camera_fb_t *fb, char *text, size_t text_size)
{
    if (!fb || !fb->buf || fb->len == 0 || fb->len > OCR_IMAGE_MAX ||
        !text || text_size < 2 || MIMO_KEY[0] == 0) return false;
    text[0] = 0;
    bool opened = false, ok = false;
    char *b64 = NULL, *request = NULL, *body = NULL, *auth = NULL;
    esp_http_client_handle_t client = NULL;

    if (fb->len > (SIZE_MAX - 4) / 4 * 3) goto cleanup;
    size_t b64_cap = 4 * ((fb->len + 2) / 3) + 1;
    b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64) goto cleanup;
    size_t b64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_len,
                              fb->buf, fb->len) != 0) goto cleanup;
    b64[b64_len] = 0;

    static const char *fmt =
        "{\"model\":\"mimo-v2.5\",\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"请识别这张绘本照片中的所有文字，只输出文字本身，按从上到下、从左到右排序；看不清的字不要猜。\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,%s\"}}]}],"
        "\"max_tokens\":512,\"temperature\":0.1}";
    int request_len = snprintf(NULL, 0, fmt, b64);
    if (request_len <= 0) goto cleanup;
    request = heap_caps_malloc((size_t)request_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) goto cleanup;
    snprintf(request, (size_t)request_len + 1, fmt, b64);
    heap_caps_free(b64); b64 = NULL;

    size_t auth_len = strlen(MIMO_KEY) + sizeof("Bearer ");
    auth = malloc(auth_len);
    if (!auth) goto cleanup;
    snprintf(auth, auth_len, "Bearer %s", MIMO_KEY);
    esp_http_client_config_t cfg = {
        .url = OCR_URL, .method = HTTP_METHOD_POST, .timeout_ms = 15000,
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
        int remaining = request_len - sent;
        int chunk = remaining > 512 ? 512 : remaining;
        int n = esp_http_client_write(client, request + sent, chunk);
        if (n <= 0) goto cleanup;
        sent += n;
        /* 手机热点/MSS=536 下避免一次性占满 Wi-Fi DMA TX buffer。 */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    heap_caps_free(request); request = NULL;
    if (esp_http_client_fetch_headers(client) < 0) goto cleanup;
    int status = esp_http_client_get_status_code(client);
    int64_t content_len = esp_http_client_get_content_length(client);
    if (content_len > OCR_RESPONSE_MAX) goto cleanup;
    body = heap_caps_malloc(OCR_RESPONSE_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) goto cleanup;
    size_t total = 0;
    while (total < OCR_RESPONSE_MAX) {
        if (content_len >= 0 && (int64_t)total == content_len) break;
        int got = esp_http_client_read(client, body + total, OCR_RESPONSE_MAX - total);
        if (got < 0) goto cleanup;
        if (got == 0) break;
        total += (size_t)got;
    }
    body[total] = 0;
    if (!esp_http_client_is_complete_data_received(client) || status != 200) {
        ESP_LOGW(TAG, "HTTP %d or truncated response (%u bytes)", status, (unsigned)total);
        goto cleanup;
    }
    ok = json_string(body, text, text_size);
    if (!ok) ESP_LOGW(TAG, "OCR response has no usable content");

cleanup:
    if (opened && client) (void)esp_http_client_close(client);
    if (client) esp_http_client_cleanup(client);
    free(auth); heap_caps_free(b64); heap_caps_free(request); heap_caps_free(body);
    return ok;
}
