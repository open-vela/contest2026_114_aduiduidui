














/**
 * 智瞳伴读 — LCD+CAM+WiFi+文字叠加 + 狐狸精灵
 * 显示: 日期时间, 智瞳, 帧率, 麦克风音量, LVGL 小狐狸
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"
#include "ping/ping_sock.h"
#include "voice.h"
#include "chat_client.h"
#include "reading_manager.h"
#include "secrets.h"
#include "lvgl.h"
#include "fox_companion.h"
#include "xp_system.h"
#include "ui_layout.h"
#include "pet_test.h"
#include "cam_stream.h"
#include "font8x16.h"

static const char *TAG = "zhitong";
#define LCD_HOST SPI2_HOST
#define LCD_SCLK 21
#define LCD_MOSI 47
#define LCD_CS   44
#define LCD_DC   43
#define LCD_RST  45
#define LCD_W    240
#define LCD_H    240   /* camera viewport height */
#define DISP_H   320   /* physical ST7789 display height */

/* ===== Globals ===== */
static esp_lcd_panel_handle_t lcd;
static volatile int mic_level = 0;
static i2s_chan_handle_t mic_chan = NULL;

/* ===== LCD Text ===== */
static void draw_char(uint16_t *fb, int x, int y, char ch, uint16_t fg, uint16_t bg) {
    int idx = (int)(unsigned char)ch - 32;
    if (idx < 0 || idx > 94) idx = 0;
    for (int r = 0; r < 16; r++) {
        uint8_t l = font8x16[idx][r];
        for (int c = 0; c < 8; c++)
            if (x+c>=0 && x+c<LCD_W && y+r>=0 && y+r<LCD_H)
                fb[(y+r)*LCD_W + (x+c)] = (l & (0x80>>c)) ? fg : bg;
    }
}
static void draw_str(uint16_t *fb, int x, int y, const char *s, uint16_t fg) {
    while (*s) { draw_char(fb, x, y, *s++, fg, 0x0000); x += 8; }
}
static void draw_vol(uint16_t *fb, int x, int y, int lvl, uint16_t fg) {
    /* Big bar: lvl 0-32767 → 0-80 pixels */
    int w = lvl * 80 / 32768;
    if (w > 80) w = 80;
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 80; c++)
            fb[(y+r)*LCD_W + (x+2+c)] = (c < w) ? fg : 0x4208;
    /* Outline */
    for (int c = 0; c < 82; c++) { fb[y*LCD_W+x+c] = 0xFFFF; fb[(y+16)*LCD_W+x+c] = 0xFFFF; }
    for (int r = 0; r < 17; r++) { fb[(y+r)*LCD_W+x] = 0xFFFF; fb[(y+r)*LCD_W+x+82] = 0xFFFF; }
}

/* ===== Mic Monitor + ASR Task ===== */
extern esp_err_t esp_crt_bundle_attach(void*);

#define MIMO_URL MIMO_API_URL

/* 大缓冲优先 PSRAM, 失败回退内部 RAM */
static void *external_or_internal_malloc(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

/* 排空 I2S 缓冲里的陈旧数据 (按键瞬间杂音/上一段残留)。
 * 必须带时间上限: mic 常开 (mic_reader 喂音量条) 数据源源不断,
 * 无上限会死循环 (9-12 实锤: 短按 ASR 卡死在 stream PAUSED 后) */
static void mic_drain(i2s_chan_handle_t ch) {
    uint8_t tmp[256];
    size_t got;
    int64_t end = esp_timer_get_time() + 300000;
    do {
        got = 0;
        esp_err_t err = i2s_channel_read(ch, tmp, sizeof(tmp), &got, pdMS_TO_TICKS(100));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) break;
    } while (got > 0 && esp_timer_get_time() < end);
}

#define ASR_SAMPLE_HZ      8000
#define ASR_PCM_BYTES      (ASR_SAMPLE_HZ * 4U)  /* 2s * 16bit 单声道 = 32KB → base64 42.7KB, 1s 传完;
                                                    16k 1.5s = 64KB 上传在慢网下卡 (校园网); 8k 长句糙,
                                                    短词 (hello/你好) OK。若网络好转可回 16000 + *3U */
#define ASR_SILENCE_PEAK   200
#define ASR_RESPONSE_MAX   4096

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_sz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_sz) {
        if (p[0] == '\\' && p[1]) {
            out[n++] = (p[1] == 'n') ? '\n' : p[1];
            p += 2;
        } else out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static void do_asr(i2s_chan_handle_t m) {
    if (!m || MIMO_KEY[0] == 0) {
        ESP_LOGE(TAG, "ASR unavailable: mic/API key not configured");
        return;
    }
    /* 回声规避: TTS 播放中跳过录音 (喇叭声会被 mic 收进去) */
    if (voice_is_playing()) { ESP_LOGW(TAG, "voice playing, skip ASR"); return; }

    /* 事务期间停视频推流 + 挂起 VU 读取: 64KB 上传与视频流抢上行曾导致
     * write 停滞; mic_reader 竞争 I2S 读锁会丢录音数据 */
    cam_stream_pause(true);
    ui_set_mic_suspend(true);

    int64_t started_us = esp_timer_get_time();
    bool success = false, opened = false;
    esp_http_client_handle_t client = NULL;
    char *request = NULL, *body = NULL, *authorization = NULL, *transcript = NULL;
    void *pcm = NULL, *wav = NULL, *base64 = NULL;

    mic_drain(m);
    ESP_LOGI(TAG, "ASR recording 2s...");
    pcm = external_or_internal_malloc(ASR_PCM_BYTES);
    if (!pcm) { ESP_LOGE(TAG, "oom ASR pcm"); goto cleanup; }
    size_t pcm_bytes = 0;
    int64_t deadline = esp_timer_get_time() + 4500000;
    while (pcm_bytes < ASR_PCM_BYTES && esp_timer_get_time() < deadline) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(m, (uint8_t *)pcm + pcm_bytes,
                                         ASR_PCM_BYTES - pcm_bytes, &got, pdMS_TO_TICKS(500));
        if (err == ESP_ERR_TIMEOUT) continue;
        if (err != ESP_OK) { ESP_LOGE(TAG, "mic read: %s", esp_err_to_name(err)); goto cleanup; }
        pcm_bytes += got;
    }
    if (pcm_bytes < ASR_PCM_BYTES) { ESP_LOGE(TAG, "short mic capture: %u/%u", (unsigned)pcm_bytes, (unsigned)ASR_PCM_BYTES); goto cleanup; }

    int peak = 0;
    for (size_t i = 0; i < pcm_bytes / sizeof(int16_t); i++) {
        int sample = ((int16_t *)pcm)[i];
        int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
    }
    mic_level = peak;
    ESP_LOGI(TAG, "ASR capture done: %u bytes, peak=%d %s",
             (unsigned)pcm_bytes, peak,
             peak < ASR_SILENCE_PEAK ? "(太安静, 判定为静音放弃)" : "(电平正常)");
    if (peak < ASR_SILENCE_PEAK) { ESP_LOGW(TAG, "ASR input too quiet (peak=%d)", peak); goto cleanup; }

    size_t wav_bytes = pcm_bytes + 44;
    wav = external_or_internal_malloc(wav_bytes);
    if (!wav) { ESP_LOGE(TAG, "oom ASR wav"); goto cleanup; }
    /* 8k 采样率 header: 0x1F40=8000, 字节率 0x3E80=16000 (8k*16bit 单声道)。
     * 之前误配 16k header → MiMo 按 2 倍速解 → "Hello" 重复三遍仍能识别 */
    static const uint8_t header[44] = {
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0,
        'd','a','t','a',0,0,0,0
    };
    memcpy(wav, header, sizeof(header));
    write_le32((uint8_t *)wav + 4, (uint32_t)wav_bytes - 8);
    write_le32((uint8_t *)wav + 40, (uint32_t)pcm_bytes);
    memcpy((uint8_t *)wav + 44, pcm, pcm_bytes);

    size_t base64_cap = 4 * ((wav_bytes + 2) / 3) + 1;
    base64 = external_or_internal_malloc(base64_cap);
    if (!base64) goto cleanup;
    size_t base64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)base64, base64_cap, &base64_len,
                              wav, wav_bytes) != 0) { ESP_LOGE(TAG, "ASR base64 encode failed"); goto cleanup; }
    ((char *)base64)[base64_len] = 0;
    free(wav);
    wav = NULL;

    static const char *request_format =
        "{\"model\":\"mimo-v2.5-asr\",\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"%s\","
        "\"format\":\"wav\"}}]}],\"max_tokens\":100}";
    int request_len = snprintf(NULL, 0, request_format, (char *)base64);
    if (request_len <= 0) goto cleanup;
    request = external_or_internal_malloc((size_t)request_len + 1);
    if (!request) goto cleanup;
    snprintf(request, (size_t)request_len + 1, request_format, (char *)base64);
    free(base64);
    base64 = NULL;

    size_t auth_len = strlen(MIMO_KEY) + sizeof("Bearer ");
    authorization = malloc(auth_len);
    if (!authorization) goto cleanup;
    snprintf(authorization, auth_len, "Bearer %s", MIMO_KEY);

    /* timeout_ms 必须在 open 前生效: 建连时写入 SO_RCVTIMEO/SO_SNDTIMEO,
     * open 后 set 只改 select 轮询改不动 socket。20s: 写阶段由 12s 零进展
     * 截止兜底, 响应阶段校园网下 MiMo 推理 >10s (曾 HTTP -1 假失败) */
    esp_http_client_config_t config = {
        .url = MIMO_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_err_t err;
    int sent = 0, retries = 0, attempt = 0;
    int64_t last_progress_ms = 0, progress_deadline_ms = 0;

    /* 分块写 4KB/块 + 50ms 间隔。WANT_WRITE(-26752) 同指针同长度重试;
     * 同一连接 12s 零进展 → 弃连接换新 HTTPS 从 0 重发 (最多 2 条连接) */
start_conn:
    attempt++;
    if (attempt > 1) {
        ESP_LOGW(TAG, "ASR conn stalled; fresh conn attempt %d", attempt);
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
        opened = false;
        vTaskDelay(pdMS_TO_TICKS(3000));   /* 等 DNS/网络恢复 */
    }
    client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "ASR http init failed"); goto cleanup; }
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    err = esp_http_client_open(client, request_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ASR open: %s", esp_err_to_name(err)); goto cleanup; }
    opened = true;
    sent = 0;
    retries = 0;
    last_progress_ms = esp_timer_get_time() / 1000;
    progress_deadline_ms = last_progress_ms + 12000;
    while (sent < request_len) {
        int chunk = request_len - sent;
        if (chunk > 4096) chunk = 4096;
        int n = esp_http_client_write(client, request + sent, chunk);
        if (n <= 0) {
            retries++;
            int64_t now_ms = esp_timer_get_time() / 1000;
            ESP_LOGW(TAG, "ASR write chunk fail: %d @%d/%d (retry %d/5, stalled %lldms, int=%u dma=%u)",
                     n, sent, request_len, retries,
                     (long long)(now_ms - last_progress_ms),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
            if (retries > 5) break;
            if (now_ms >= progress_deadline_ms) {
                ESP_LOGW(TAG, "ASR write: 12s zero progress, dropping conn");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        retries = 0;
        sent += n;
        last_progress_ms = esp_timer_get_time() / 1000;
        progress_deadline_ms = last_progress_ms + 12000;
        if (sent % 16384 == 0 || sent == request_len)
            ESP_LOGI(TAG, "ASR write: %d/%d", sent, request_len);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (sent != request_len && attempt < 2) goto start_conn;
    if (sent != request_len) { ESP_LOGE(TAG, "ASR write: %d/%d", sent, request_len); goto cleanup; }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *content_type = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK &&
        content_type && strncmp(content_type, "application/json", 16) != 0) {
        ESP_LOGE(TAG, "unexpected ASR Content-Type: %s", content_type);
        goto cleanup;
    }
    int64_t response_length = esp_http_client_get_content_length(client);
    if (response_length > (int64_t)ASR_RESPONSE_MAX) {
        ESP_LOGE(TAG, "ASR response too large: %lld", (long long)response_length);
        goto cleanup;
    }
    body = external_or_internal_malloc(ASR_RESPONSE_MAX + 1);
    if (!body) goto cleanup;
    size_t total = 0;
    while (total < ASR_RESPONSE_MAX) {
        int got = esp_http_client_read(client, body + total, ASR_RESPONSE_MAX - total);
        if (got < 0) { ESP_LOGE(TAG, "ASR response read failed"); goto cleanup; }
        if (got == 0) break;
        total += (size_t)got;
    }
    body[total] = 0;

    if (status != 200) {
        ESP_LOGE(TAG, "ASR HTTP %d: %.300s", status, total ? body : "<empty>");
        goto cleanup;
    }
    if (total == ASR_RESPONSE_MAX) { ESP_LOGE(TAG, "ASR response reached limit"); goto cleanup; }

    /* transcript 从栈挪到堆: do_asr 跑在 mic_asr 任务栈上(曾打穿 32KB 栈
     * 触发 newlib vfprintf abort → 重启), 局部大数组能省则省 */
    transcript = malloc(1024);
    if (!transcript) { ESP_LOGE(TAG, "oom ASR transcript"); goto cleanup; }
    if (!json_extract_string(body, "content", transcript, 1024) || transcript[0] == 0) {
        ESP_LOGE(TAG, "ASR response has no message.content: %.300s", body);
        goto cleanup;
    }
    ESP_LOGI(TAG, "---------- 语音对话 ----------");
    ESP_LOGI(TAG, "[听到] %s", transcript);
    free(body);
    body = NULL;

    fox_handle_event(FOX_EVENT_QUESTION);
    char reply[768];
    if (!chat_request_reply(transcript, reply, sizeof(reply))) {
        fox_handle_event(FOX_EVENT_ANSWER_ERROR);
        ESP_LOGW(TAG, "[回应] Chat 请求失败, 无 TTS 回复 (详见 Chat 标签的原始返回)");
        ESP_LOGI(TAG, "------------------------------");
        goto cleanup;
    }
    ESP_LOGI(TAG, "[回应] %s", reply);
    ESP_LOGI(TAG, "------------------------------");
    voice_speak_async(reply);
    add_xp(DIALOG_XP);
    fox_handle_event(FOX_EVENT_ANSWER_SUCCESS);
    success = true;

cleanup:
    if (opened && client) (void)esp_http_client_close(client);
    if (client) esp_http_client_cleanup(client);
    free(body);
    free(authorization);
    free(request);
    free(base64);
    free(transcript);
    free(pcm);
    cam_stream_pause(false);
    ui_set_mic_suspend(false);
    ESP_LOGI(TAG, "ASR %s in %lld ms, int_free=%u, spi_free=%u, dma_free=%u",
             success ? "OK" : "FAILED",
             (long long)((esp_timer_get_time() - started_us) / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

static void mic_monitor(void *arg) {
    i2s_chan_handle_t m = (i2s_chan_handle_t)arg;

    /* Button */
    gpio_config_t bc = { .pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&bc);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(GPIO_NUM_0)) continue;
        vTaskDelay(pdMS_TO_TICKS(80));
        if (gpio_get_level(GPIO_NUM_0)) continue;

        ESP_LOGI(TAG, "BOOT pressed!");
        int64_t pressed_us = esp_timer_get_time();
        while (!gpio_get_level(GPIO_NUM_0) &&
               esp_timer_get_time() - pressed_us < 10000000) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        int64_t held_us = esp_timer_get_time() - pressed_us;
        if (held_us >= 1200000) {
            ESP_LOGI(TAG, "long press -> OCR reading");
            (void)reading_capture_and_start();
        } else {
            do_asr(m);
        }
        while (!gpio_get_level(GPIO_NUM_0)) vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* ===== WiFi 健康看门狗 =====
 * 假死症状: 系统正常(屏幕在走)但网络全断, 驱动层卡死且无断开事件。
 * 每 10s ping 一次网关; 连续 5 次失败(≈50s)判定假死 → 重启 WiFi 驱动自救。
 * 阈值取 5 是为了容忍正常死区(实测最长 ~30s 自动恢复)。 */
static volatile int g_ping_done = 0;
static volatile int g_ping_fail = 0;
static esp_ping_handle_t s_ping = NULL;   /* session 常驻复用, 不反复 create/delete (碎片) */

static void ping_ok_cb(esp_ping_handle_t h, void *arg) { (void)h; (void)arg; g_ping_fail = 0; g_ping_done = 1; }
static void ping_to_cb(esp_ping_handle_t h, void *arg) { (void)h; (void)arg; g_ping_fail++; g_ping_done = 1; }
static void ping_end_cb(esp_ping_handle_t h, void *arg) { (void)h; (void)arg; g_ping_done = 1; }

static void wifi_watchdog_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));   /* 等首次联网 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!n) continue;
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.ip.addr == 0) continue;
        esp_ping_callbacks_t cbs = { .on_ping_success = ping_ok_cb,
                                     .on_ping_timeout = ping_to_cb,
                                     .on_ping_end = ping_end_cb };
        esp_ping_config_t cfg = { .count = 1, .interval_ms = 500,
                                  .timeout_ms = 2000, .data_size = 0,
                                  .task_stack_size = 8192,   /* 2048 实测在 ASR 上传高负载时栈溢出 → 整机重启 */
                                  .target_addr.type = IPADDR_TYPE_V4,
                                  .target_addr.u_addr.ip4.addr = ip.gw.addr };
        if (!s_ping && esp_ping_new_session(&cfg, &cbs, &s_ping) != ESP_OK) {
            ESP_LOGW(TAG, "ping session fail: int free=%u max=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            continue;   /* 创建失败下轮重试; session 常驻, 不反复 create/delete (防碎片) */
        }
        g_ping_done = 0;
        esp_ping_start(s_ping);
        for (int i = 0; i < 60 && !g_ping_done; i++) vTaskDelay(pdMS_TO_TICKS(50));
        if (g_ping_fail >= 5) {
            ESP_LOGW(TAG, "WiFi watchdog: 网关连续 %d 次 ping 失败, 假死! 重启 WiFi 驱动", g_ping_fail);
            g_ping_fail = 0;
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_start();   /* 触发 WIFI_EVENT_STA_START → wcb 自动重连 */
            vTaskDelay(pdMS_TO_TICKS(20000));   /* 等重连稳定 */
        }
    }
}

/* ===== WiFi ===== */
static void wcb(void *a, esp_event_base_t b, int32_t c, void *d) {
    if (b == WIFI_EVENT && c == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (b == WIFI_EVENT && c == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (b == IP_EVENT && c == IP_EVENT_STA_GOT_IP) {
        esp_ip4_addr_t *addr = &((ip_event_got_ip_t*)d)->ip_info.ip;
        ESP_LOGI(TAG, "WiFi: " IPSTR, IP2STR(addr));
        char buf[32];
        snprintf(buf, sizeof(buf), "http://" IPSTR "/", IP2STR(addr));
        ui_set_ip_text(buf);
    }
}

/* 开机问候: 独立任务轮询等 WiFi 就绪 (最多 15s), 不阻塞 app_main/UI;
 * 离线时也会在 15s 后播 (TTS 失败仅打日志, 无副作用) */
static void greeting_task(void *arg) {
    (void)arg;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < 150; i++) {
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* 先等开机提示音播完 (最多 20s), 别抢断它; 超时则照说, 抢断模型兜底 */
    (void)voice_wait_idle(20000);
    voice_speak_async("你好，小朋友，我是智瞳伴读小狐狸");
    printf("你好，小朋友，我是智瞳伴读小狐狸\n");
    vTaskDelete(NULL);
}

/* ===== Camera (JPEG 320x240, 低延迟) ===== */
static esp_err_t cam_init(void) {
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 15, .pin_sccb_sda = 4, .pin_sccb_scl = 5,
        .pin_d7 = 16, .pin_d6 = 17, .pin_d5 = 18, .pin_d4 = 12,
        .pin_d3 = 10, .pin_d2 = 8,  .pin_d1 = 9,  .pin_d0 = 11,
        .pin_vsync = 6, .pin_href = 7, .pin_pclk = 13,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_QVGA,  /* 320x240 JPEG */
        .jpeg_quality = 25, .fb_count = 3, .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location = CAMERA_FB_IN_PSRAM,  /* 用 PSRAM 存大帧 */
    };
    esp_err_t r = esp_camera_init(&c);
    if (r == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        s->set_vflip(s, 1);
        /* 关白平衡/增益自动 (减 CPU) */
        s->set_whitebal(s, 0);
        s->set_gain_ctrl(s, 0);
        s->set_aec2(s, 0);
    }
    return r;
}

/* ===== LVGL Display Flush ===== */
static void lv_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    esp_lcd_panel_draw_bitmap(lcd, area->x1, area->y1, area->x1 + w, area->y1 + h, color_p);
    lv_disp_flush_ready(drv);
}

/* LVGL tick timer (5ms) */
static void lv_tick_cb(void *arg) { lv_tick_inc(5); }

/* ===== Main ===== */
void app_main(void) {
    nvs_flash_init();
    esp_netif_init(); esp_event_loop_create_default(); esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&wc);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wcb, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wcb, NULL, NULL);
    wifi_config_t ws = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &ws); esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);  /* 关省电: 省电模式会导致 TCP 数据延迟/丢包 */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL); esp_sntp_setservername(0, "pool.ntp.org"); esp_sntp_init();
    setenv("TZ", "CST-8", 1); tzset();

    spi_bus_config_t bus = { .sclk_io_num = LCD_SCLK, .mosi_io_num = LCD_MOSI, .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_W * DISP_H * 2 };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io = { .cs_gpio_num = LCD_CS, .dc_gpio_num = LCD_DC, .spi_mode = 0, .pclk_hz = 20*1000*1000, .trans_queue_depth = 10, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    esp_lcd_panel_io_handle_t ih;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((void*)LCD_HOST, &io, &ih));
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ih, &pc, &lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd)); ESP_ERROR_CHECK(esp_lcd_panel_init(lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd, true)); ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd, true));
    ESP_ERROR_CHECK(cam_init());

    /* ---- LVGL Init ---- */
    lv_init();
    esp_timer_handle_t lv_tick_timer;
    esp_timer_create_args_t tick_args = { .callback = lv_tick_cb, .name = "lv_tick" };
    esp_timer_create(&tick_args, &lv_tick_timer);
    esp_timer_start_periodic(lv_tick_timer, 5000); /* 5ms */
    static lv_color_t lv_buf1[LCD_W * 20];
    static lv_color_t lv_buf2[LCD_W * 20];
    lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, lv_buf1, lv_buf2, LCD_W * 20);
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_W;
    disp_drv.ver_res = DISP_H;
    disp_drv.flush_cb = lv_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* ---- I2S Mic Init (共享通道) ---- */
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&cc, NULL, &mic_chan);
    i2s_std_config_t sc = { .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ASR_SAMPLE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .bclk = GPIO_NUM_41, .ws = GPIO_NUM_42, .din = GPIO_NUM_2,
            .dout = I2S_GPIO_UNUSED, .mclk = I2S_GPIO_UNUSED,
            .invert_flags = { .bclk_inv = false, .ws_inv = false } } };
    i2s_channel_init_std_mode(mic_chan, &sc);

    /* ---- Voice/TTS Init (I2S1 + MAX98357A) ---- */
    voice_init();
    voice_play_boot_chime();   /* 开机提示音: 非阻塞, voice_task 后台播, 兼喇叭硬件自检 */
    xTaskCreate(greeting_task, "greeting", 4096, NULL, 2, NULL);   /* 开机问候: 等 WiFi 后播 */

    /* ---- XP System Init ---- */
    xp_system_init();

    /* ---- Fox Init (先创建狐狸) ---- */
    lv_fox_init(NULL);

    /* ---- UI Layout (日期/时间/Lv/XP/音量, NTP 后台异步同步) ---- */
    ui_layout_init(mic_chan);

    if (!reading_manager_init())
        ESP_LOGE(TAG, "reading manager init failed; OCR/25min reminder disabled");

    /* Mic monitor task (按键 ASR, 与 mic_reader 共享 I2S) */
    /* 32KB 不够: do_asr(录音+ASR+TLS+Chat+TTS排队+XP日志) 全压这个栈上,
     * 9-12 实测 87% 高水位 + add_xp 打日志时 vfprintf abort → 重启 */
    xTaskCreate(mic_monitor, "mic_asr", 49152, (void *)mic_chan, 2, NULL);

    /* WiFi 健康看门狗 (假死自动重启 WiFi 驱动) */
    xTaskCreate(wifi_watchdog_task, "wifi_wdt", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "RUNNING — ZhiTong AI Companion");

    /* ---- WebSocket 视频推流 (JPEG 320x240, ~10fps) ---- */
    cam_stream_init();

    /* ---- 主循环: LVGL 渲染 + 宠物逻辑测试 ---- */
    while (1) {
        lv_timer_handler();
        pet_logic_test_update();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
