/**
 * voice.c — TTS 语音输出 (voice_task + I2S1 + MAX98357A)
 * ========================================================
 * 链路: 文本 → MiMo TTS HTTP → JSON/base64 WAV → PCM → I2S1 → MAX98357A → 喇叭
 * 引脚: I2S1 复用 SD 卡槽三根线: BCLK=IO39, WS=IO38, DOUT=IO40。
 *
 * 抢断模型: 每次 speak/interrupt 都递增 generation。旧事务会在 HTTP 收包、
 * 解码前和每个 PCM 块边界检测 generation，避免 bool stop 在新旧事务间竞态。
 */

#include "voice.h"
#include "secrets.h"   /* MIMO_KEY — gitignored, 勿提交 */
#include "cam_stream.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "Voice";

#define VOICE_I2S_PORT          I2S_NUM_1
#define VOICE_SAMPLE_HZ         24000
#define VOICE_CHUNK_SMP         512
#define VOICE_TASK_STACK        8192
#define VOICE_TASK_PRIO         3
#define VOICE_TEXT_BYTES        512
#define TTS_HTTP_BUFFER_BYTES   2048
#define TTS_RESPONSE_INITIAL    (64U * 1024U)
#define TTS_RESPONSE_MAX        (1024U * 1024U)

#define VOICE_DEFAULT_VOLUME    50

#define MIMO_URL   MIMO_API_URL
#define MIMO_VOICE "冰糖"

typedef enum {
    VOICE_MSG_TEXT = 0,   /* MiMo TTS 播报 */
    VOICE_MSG_CHIME,      /* 本地合成开机提示音 (不依赖网络) */
    VOICE_MSG_TEST_TONE,  /* 1kHz 硬件哑测音 */
} voice_msg_kind_t;

typedef struct {
    voice_msg_kind_t kind;
    char text[VOICE_TEXT_BYTES];
    uint32_t generation;
} voice_msg_t;

typedef struct {
    uint16_t format;
    uint16_t channels;
    uint16_t bits;
    uint32_t sample_rate;
    size_t data_offset;
    size_t data_size;
} wav_info_t;

static QueueHandle_t s_queue = NULL;
static i2s_chan_handle_t s_tx = NULL;
static volatile bool s_playing = false;
static volatile bool s_busy = false;
static volatile bool s_pending = false;
static volatile uint8_t s_volume = VOICE_DEFAULT_VOLUME;
static volatile uint32_t s_generation = 0;
static portMUX_TYPE s_generation_mux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t voice_next_generation(void)
{
    uint32_t value;
    portENTER_CRITICAL(&s_generation_mux);
    value = ++s_generation;
    if (value == 0) value = ++s_generation;
    portEXIT_CRITICAL(&s_generation_mux);
    return value;
}

static uint32_t voice_current_generation(void)
{
    uint32_t value;
    portENTER_CRITICAL(&s_generation_mux);
    value = s_generation;
    portEXIT_CRITICAL(&s_generation_mux);
    return value;
}

static bool voice_cancelled(uint32_t generation)
{
    return generation != voice_current_generation();
}

static void voice_set_active(bool active)
{
    portENTER_CRITICAL(&s_generation_mux);
    s_playing = active;
    s_busy = active;
    portEXIT_CRITICAL(&s_generation_mux);
}

static void voice_set_pending(bool pending)
{
    portENTER_CRITICAL(&s_generation_mux);
    s_pending = pending;
    portEXIT_CRITICAL(&s_generation_mux);
}

static void voice_i2s_deinit(void)
{
    if (!s_tx) return;
    (void)i2s_channel_disable(s_tx);
    (void)i2s_del_channel(s_tx);
    s_tx = NULL;
}

static esp_err_t voice_i2s_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(VOICE_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&cc, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        s_tx = NULL;
        return err;
    }

    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = GPIO_NUM_39,
            .ws = GPIO_NUM_38,
            .dout = GPIO_NUM_40,
            .din = I2S_GPIO_UNUSED,
            .mclk = I2S_GPIO_UNUSED,
            .invert_flags = { .bclk_inv = false, .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &sc);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S1 init/enable: %s", esp_err_to_name(err));
        voice_i2s_deinit();
        return err;
    }
    /* 保持通道常开 (时钟常跑): 与 9-12 验证出声的旧固件行为一致。
       9-15 实测: "空闲停时钟 + 播放再开" 的省电写法在这块 MAX98357A
       模组上不响 —— 模组对时钟停止/恢复不可靠, 常开才是稳的 */

    ESP_LOGI(TAG, "I2S1 ready (bclk=39 ws=38 dout=40, %u/16b/mono), volume=%u%%",
             (unsigned)VOICE_SAMPLE_HZ, (unsigned)s_volume);
    return ESP_OK;
}

/* 播放结束收尾: 停再开, 只弹一下冲掉 DMA 环里的残留, 时钟保持常跑
   (与 9-12 验证出声的旧固件一致; 停在常开模组上不响, 见 voice_i2s_init) */
static void voice_flush_dma(void)
{
    if (!s_tx) return;
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) ESP_LOGW(TAG, "I2S DMA flush: %s", esp_err_to_name(err));
}

static char *json_escape_alloc(const char *src)
{
    size_t input_len = strlen(src);
    if (input_len > (SIZE_MAX - 1) / 6) return NULL;
    char *dst = malloc(input_len * 6 + 1);
    if (!dst) return NULL;

    static const char hex[] = "0123456789abcdef";
    size_t out = 0;
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            dst[out++] = '\\';
            dst[out++] = c == '\b' ? 'b' : c == '\f' ? 'f' :
                         c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        } else if (c < 0x20) {
            dst[out++] = '\\'; dst[out++] = 'u'; dst[out++] = '0'; dst[out++] = '0';
            dst[out++] = hex[c >> 4]; dst[out++] = hex[c & 0x0f];
        } else {
            dst[out++] = (char)c;
        }
    }
    dst[out] = 0;
    return dst;
}

static bool wav_parse(const uint8_t *wav, size_t len, wav_info_t *info)
{
    if (!wav || !info || len < 12 || memcmp(wav, "RIFF", 4) ||
        memcmp(wav + 8, "WAVE", 4)) return false;

    memset(info, 0, sizeof(*info));
    bool have_fmt = false;
    bool have_data = false;
    size_t off = 12;

    while (off <= len - 8) {
        uint32_t chunk_size = read_le32(wav + off + 4);
        size_t payload = off + 8;
        if ((size_t)chunk_size > len - payload) return false;

        if (memcmp(wav + off, "fmt ", 4) == 0) {
            if (chunk_size < 16) return false;
            info->format = read_le16(wav + payload);
            info->channels = read_le16(wav + payload + 2);
            info->sample_rate = read_le32(wav + payload + 4);
            info->bits = read_le16(wav + payload + 14);
            have_fmt = true;
        } else if (memcmp(wav + off, "data", 4) == 0) {
            info->data_offset = payload;
            info->data_size = chunk_size;
            have_data = true;
        }

        size_t padded = (size_t)chunk_size + (chunk_size & 1U);
        if (padded > len - payload) {
            if (have_data && info->data_offset + info->data_size == len) break;
            return false;
        }
        off = payload + padded;
    }

    return have_fmt && have_data && info->format == 1 &&
           info->channels == 1 && info->bits == 16 &&
           info->sample_rate == VOICE_SAMPLE_HZ &&
           (info->data_size % sizeof(int16_t) == 0);
}

static char *json_find_string_value(char *json, const char *key)
{
    char pattern[32];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) return NULL;

    char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p == '"' ? p + 1 : NULL;
}

static esp_err_t tts_read_body(esp_http_client_handle_t client, uint32_t generation,
                               uint8_t **out_buf, size_t *out_len)
{
    int64_t content_len = esp_http_client_get_content_length(client);
    if (content_len > (int64_t)TTS_RESPONSE_MAX) {
        ESP_LOGE(TAG, "TTS response too large: %lld", (long long)content_len);
        return ESP_ERR_NO_MEM;
    }

    size_t cap = TTS_RESPONSE_INITIAL + 1;
    if (content_len > 0 && (size_t)content_len + 1 > cap) cap = (size_t)content_len + 1;

    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t total = 0;
    esp_err_t result = ESP_OK;
    while (!voice_cancelled(generation)) {
        if (total == cap - 1) {
            if (cap - 1 >= TTS_RESPONSE_MAX) {
                result = ESP_ERR_NO_MEM;
                ESP_LOGE(TAG, "TTS response exceeded %u bytes", (unsigned)TTS_RESPONSE_MAX);
                break;
            }
            size_t new_cap = (cap - 1) * 2 + 1;
            if (new_cap - 1 > TTS_RESPONSE_MAX) new_cap = TTS_RESPONSE_MAX + 1;
            uint8_t *grown = heap_caps_realloc(
                buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) {
                result = ESP_ERR_NO_MEM;
                break;
            }
            buf = grown;
            cap = new_cap;
        }

        int got = esp_http_client_read(client, (char *)buf + total, cap - total - 1);
        if (got < 0) {
            result = ESP_FAIL;
            break;
        }
        if (got == 0) break;
        total += (size_t)got;
    }

    if (voice_cancelled(generation)) result = ESP_ERR_INVALID_STATE;
    if (result != ESP_OK) {
        heap_caps_free(buf);
        return result;
    }

    buf[total] = 0;
    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

static bool play_pcm_blocks(const int16_t *pcm, size_t bytes, uint32_t generation)
{
    /* 上一次播放结束已停通道(消除空闲杂音); 播放前重新使能。
       INVALID_STATE 表示通道已处于使能态, 属正常。 */
    esp_err_t en = i2s_channel_enable(s_tx);
    if (en != ESP_OK && en != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s enable: %s", esp_err_to_name(en));
    }

    size_t off = 0;
    bool completed = true;

    while (off < bytes) {
        if (voice_cancelled(generation)) {
            ESP_LOGI(TAG, "interrupted @%u/%u", (unsigned)off, (unsigned)bytes);
            completed = false;
            break;
        }

        size_t count = bytes - off > VOICE_CHUNK_SMP * 2 ?
                       VOICE_CHUNK_SMP * 2 : bytes - off;
        int16_t tmp[VOICE_CHUNK_SMP];
        size_t samples = count / sizeof(int16_t);
        uint8_t volume = s_volume;
        for (size_t i = 0; i < samples; i++) {
            tmp[i] = (int16_t)(((int32_t)pcm[off / 2 + i] * volume) / 100);
        }

        size_t written = 0;
        esp_err_t err = i2s_channel_write(
            s_tx, tmp, count, &written, pdMS_TO_TICKS(250));
        if (err != ESP_OK || written != count) {
            ESP_LOGE(TAG, "i2s write: %s, %u/%u", esp_err_to_name(err),
                     (unsigned)written, (unsigned)count);
            completed = false;
            break;
        }
        off += count;
    }

    /* 正常播完: 追加约 100ms 静音(5×512 样本@24kHz≈107ms), 覆盖 DMA ring
       (~60ms) 里的尾部残留, 避免最后一帧被重复播放; 随后停通道让功放静音。
       中断/出错时不写静音, 直接停通道以立即掐断声音。 */
    if (completed) {
        int16_t silence[VOICE_CHUNK_SMP];
        memset(silence, 0, sizeof(silence));
        for (int i = 0; i < 5; i++) {
            size_t w = 0;
            i2s_channel_write(s_tx, silence, sizeof(silence), &w,
                              pdMS_TO_TICKS(250));
        }
    }
    voice_flush_dma();
    return completed;
}

/* ---- 开机提示音 / 硬件哑测音 (本地合成, 不依赖网络) ----
 * 旧版 voice_test_tone 在 app_main 里直接 i2s_channel_write, 三个坑:
 *   1. portMAX_DELAY + 通道未使能 → 写信号量永等 → 整机卡死 (9-14 雪花屏根因)
 *   2. 阻塞 app_main 3 秒, UI/RUNNING 全部延后
 *   3. 与 voice_task 双写者竞态: WiFi 快时问候 TTS 与测试音交错写 I2S1,
 *      测试音收尾 flush 停通道会掐死正在播的问候
 * 现全部收编为: 合成 PCM → 复用 play_pcm_blocks (单写者 voice_task +
 * 250ms 有界写 + 结尾静音收尾), 永不阻塞调用方。 */

static void play_test_tone_pcm(uint32_t generation)
{
    /* 1kHz 正弦 1s, 固定大声, 验喇叭硬件不随 s_volume 走 */
    const size_t samples = (size_t)VOICE_SAMPLE_HZ * 1000 / 1000;
    const size_t fade = (size_t)VOICE_SAMPLE_HZ * 5 / 1000;   /* 5ms 淡入淡出防爆音 */

    int16_t *buf = heap_caps_malloc(samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "oom test tone"); return; }
    for (size_t i = 0; i < samples; i++) {
        float env = 1.0f;
        if (i < fade) env = (float)i / (float)fade;
        else if (i >= samples - fade) env = (float)(samples - i) / (float)fade;
        buf[i] = (int16_t)(sinf(2.0f * (float)M_PI * 1000.0f * (float)i /
                                VOICE_SAMPLE_HZ) * 12000.0f * env);
    }
    ESP_LOGI(TAG, "P0: 1kHz tone 1s (via voice_task)...");
    play_pcm_blocks(buf, samples * sizeof(int16_t), generation);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "P0: tone done");
}

static void play_boot_chime_pcm(uint32_t generation)
{
    /* C5-E5-G5-C6 上行琶音, 每音 250ms, 合计 1s; 基频+25% 二次谐波,
       5ms 淡入淡出; /1.25 防谐波叠加削顶。
       幅度固定 30000/1.25≈全响度, 不随 s_volume 走: 开机音兼硬件自检,
       必须排除"音量没开"这个变量 (2026-09-15 用户反映听不到, 先拉满) */
    static const float note_hz[4] = { 523.25f, 659.25f, 783.99f, 1046.50f };
    const size_t note_samples = (size_t)VOICE_SAMPLE_HZ * 250 / 1000;
    const size_t fade = (size_t)VOICE_SAMPLE_HZ * 5 / 1000;

    int16_t *buf = heap_caps_malloc(note_samples * 4 * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "oom boot chime"); return; }
    for (int n = 0; n < 4; n++) {
        for (size_t i = 0; i < note_samples; i++) {
            float t = (float)i / (float)VOICE_SAMPLE_HZ;
            float env = 1.0f;
            if (i < fade) env = (float)i / (float)fade;
            else if (i >= note_samples - fade) env = (float)(note_samples - i) / (float)fade;
            float s = sinf(2.0f * (float)M_PI * note_hz[n] * t) +
                      0.25f * sinf(2.0f * (float)M_PI * 2.0f * note_hz[n] * t);
            buf[n * note_samples + i] =
                (int16_t)(s * env * (30000.0f / 1.25f));
        }
    }
    ESP_LOGI(TAG, "P0: boot chime 1s (fixed loud)..." );
    play_pcm_blocks(buf, note_samples * 4 * sizeof(int16_t), generation);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "P0: chime done");
}

static void voice_play_text(const char *text, uint32_t generation)
{
    extern esp_err_t esp_crt_bundle_attach(void *);
    char *escaped = NULL;
    char *request = NULL;
    char *authorization = NULL;
    uint8_t *response = NULL;
    uint8_t *wav = NULL;
    esp_http_client_handle_t client = NULL;
    bool opened = false;
    size_t response_len = 0;

    if (MIMO_KEY[0] == 0) {
        ESP_LOGE(TAG, "MIMO_KEY is empty");
        return;
    }

    /* TTS 下载/播放期间停视频推流: 下行 205KB + 视频上行抢 2.4G 带宽 */
    cam_stream_pause(true);

    ESP_LOGI(TAG, "heap@tts: int free=%u max=%u | spi free=%u max=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    escaped = json_escape_alloc(text);
    if (!escaped) {
        ESP_LOGE(TAG, "oom/overflow escaping request");
        goto cleanup;
    }

    const char *request_format =
        "{\"model\":\"mimo-v2.5-tts\",\"messages\":[{\"role\":\"assistant\","
        "\"content\":\"%s\"}],\"audio\":{\"format\":\"wav\",\"voice\":\"" MIMO_VOICE "\"}}";
    int request_len = snprintf(NULL, 0, request_format, escaped);
    if (request_len <= 0) goto cleanup;
    request = malloc((size_t)request_len + 1);
    if (!request) goto cleanup;
    snprintf(request, (size_t)request_len + 1, request_format, escaped);

    size_t auth_len = strlen(MIMO_KEY) + sizeof("Bearer ");
    authorization = malloc(auth_len);
    if (!authorization) goto cleanup;
    snprintf(authorization, auth_len, "Bearer %s", MIMO_KEY);

    if (voice_cancelled(generation)) goto cleanup;

    ESP_LOGI(TAG, "========== TTS 请求 ==========");
    ESP_LOGI(TAG, "TTS 待播报文本: %s", text ? text : "(null)");
    ESP_LOGI(TAG, "TTS POST %s", MIMO_URL);
    ESP_LOGI(TAG, "TTS 请求体(%d字节): %s", request_len, request);

    esp_http_client_config_t config = {
        .url = MIMO_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = TTS_HTTP_BUFFER_BYTES,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http init failed");
        goto cleanup;
    }
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    int64_t started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, request_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS open: %s", esp_err_to_name(err));
        goto cleanup;
    }
    opened = true;

    int sent = esp_http_client_write(client, request, request_len);
    if (sent != request_len) {
        ESP_LOGE(TAG, "TTS write: %d/%d", sent, request_len);
        goto cleanup;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *content_type = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK &&
        content_type && strncmp(content_type, "application/json", 16) != 0) {
        ESP_LOGE(TAG, "unexpected TTS Content-Type: %s", content_type);
        goto cleanup;
    }

    err = tts_read_body(client, generation, &response, &response_len);
    ESP_LOGI(TAG, "TTS HTTP %d, %u bytes in %lld ms", status,
             (unsigned)response_len,
             (long long)((esp_timer_get_time() - started_us) / 1000));
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "TTS request cancelled");
        goto cleanup;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS read: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "TTS error body: %.240s", response_len ? (char *)response : "<empty>");
        goto cleanup;
    }
    if (voice_cancelled(generation)) goto cleanup;

    ESP_LOGI(TAG, "========== TTS 回复 ==========");
    ESP_LOGI(TAG, "TTS 回复: HTTP %d, 共 %u 字节", status, (unsigned)response_len);
    ESP_LOGI(TAG, "TTS 回复头部: %.200s", response_len ? (char *)response : "<empty>");

    char *audio = strstr((char *)response, "\"audio\"");
    char *base64 = audio ? json_find_string_value(audio, "data") : NULL;
    if (!base64) {
        ESP_LOGE(TAG, "no audio.data in TTS response");
        goto cleanup;
    }
    size_t base64_len = 0;
    while (base64 + base64_len < (char *)response + response_len &&
           base64[base64_len] != '"') base64_len++;
    if (!base64_len || base64 + base64_len >= (char *)response + response_len) {
        ESP_LOGE(TAG, "truncated/empty audio.data");
        goto cleanup;
    }
    ESP_LOGI(TAG, "TTS 命中 audio.data, base64 长度=%u 字节", (unsigned)base64_len);

    size_t wav_cap = (base64_len / 4) * 3 + 3;
    wav = heap_caps_malloc(wav_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        ESP_LOGE(TAG, "oom wav %u", (unsigned)wav_cap);
        goto cleanup;
    }
    size_t wav_len = 0;
    if (mbedtls_base64_decode(wav, wav_cap, &wav_len,
                              (const unsigned char *)base64, base64_len) != 0) {
        ESP_LOGE(TAG, "base64 decode failed (%u)", (unsigned)base64_len);
        goto cleanup;
    }
    ESP_LOGI(TAG, "TTS base64 解码完成: WAV %u 字节", (unsigned)wav_len);
    heap_caps_free(response);
    response = NULL;

    if (voice_cancelled(generation)) goto cleanup;
    wav_info_t info;
    if (!wav_parse(wav, wav_len, &info)) {
        ESP_LOGE(TAG, "unsupported/bad WAV (require PCM, 24kHz, mono, 16-bit)");
        goto cleanup;
    }
    ESP_LOGI(TAG, "PCM: %u Hz, %uch, %ubit, %u bytes",
             (unsigned)info.sample_rate, info.channels, info.bits,
             (unsigned)info.data_size);
    uint32_t dur_ms = info.sample_rate ?
        (uint32_t)((uint64_t)info.data_size * 1000U /
                   (info.sample_rate * info.channels * (info.bits / 8U))) : 0;
    ESP_LOGI(TAG, "TTS 音频时长约 %lu ms, 开始播放...", (unsigned long)dur_ms);
    int64_t play_start = esp_timer_get_time();
    bool played = play_pcm_blocks((const int16_t *)(wav + info.data_offset),
                                  info.data_size, generation);
    ESP_LOGI(TAG, "========== TTS 播放%s (耗时 %lld ms, 音频 %lu ms) ==========",
             played ? "完成 ✅ 整句播完" : "中断 ⚠️ 被抢断/取消",
             (long long)((esp_timer_get_time() - play_start) / 1000),
             (unsigned long)dur_ms);

cleanup:
    if (opened && client) (void)esp_http_client_close(client);
    if (client) esp_http_client_cleanup(client);
    if (response) heap_caps_free(response);
    if (wav) heap_caps_free(wav);
    free(authorization);
    free(request);
    free(escaped);
    cam_stream_pause(false);
}

static void voice_task(void *arg)
{
    (void)arg;
    voice_msg_t message;
    while (true) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        if (voice_cancelled(message.generation)) {
            continue;
        }

        voice_set_pending(false);
        voice_set_active(true);
        switch (message.kind) {
        case VOICE_MSG_CHIME:      play_boot_chime_pcm(message.generation); break;
        case VOICE_MSG_TEST_TONE:  play_test_tone_pcm(message.generation);  break;
        default:                   voice_play_text(message.text, message.generation); break;
        }
        voice_set_active(false);
        ESP_LOGI(TAG, "voice done, stack high-water(raw)=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

void voice_init(void)
{
    if (s_queue) {
        ESP_LOGW(TAG, "already initialized");
        return;
    }
    if (voice_i2s_init() != ESP_OK) return;

    s_queue = xQueueCreate(1, sizeof(voice_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue create failed");
        voice_i2s_deinit();
        return;
    }

    if (xTaskCreate(voice_task, "voice", VOICE_TASK_STACK, NULL,
                    VOICE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "voice task create failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        voice_i2s_deinit();
        return;
    }
    ESP_LOGI(TAG, "Ready");
}

static size_t copy_utf8_truncated(char *dst, size_t dst_size, const char *src)
{
    if (!dst_size) return 0;
    size_t max_bytes = dst_size - 1;
    size_t bytes = strnlen(src, max_bytes);
    if (src[bytes] != 0) {
        while (bytes > 0 && (((unsigned char)src[bytes] & 0xc0U) == 0x80U)) bytes--;
    }
    memcpy(dst, src, bytes);
    dst[bytes] = 0;
    return bytes;
}

void voice_speak_async(const char *text)
{
    if (!s_queue || !text || !text[0]) return;

    voice_msg_t message = { 0 };
    message.kind = VOICE_MSG_TEXT;
    size_t copied = copy_utf8_truncated(message.text, sizeof(message.text), text);
    if (!copied) return;
    if (text[copied] != 0) ESP_LOGW(TAG, "TTS text truncated to %u bytes", (unsigned)copied);

    message.generation = voice_next_generation();
    voice_set_pending(true);
    if (xQueueOverwrite(s_queue, &message) != pdPASS) {
        voice_set_pending(false);
        ESP_LOGE(TAG, "voice queue overwrite failed");
    }
}

void voice_interrupt(void)
{
    if (!s_queue) return;
    (void)voice_next_generation();
    (void)xQueueReset(s_queue);
    voice_set_pending(false);
}

void voice_set_volume(uint8_t volume)
{
    s_volume = volume > 100 ? 100 : volume;
}

bool voice_is_playing(void)
{
    bool playing;
    portENTER_CRITICAL(&s_generation_mux);
    playing = s_playing;
    portEXIT_CRITICAL(&s_generation_mux);
    return playing;
}

bool voice_wait_idle(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        bool busy;
        portENTER_CRITICAL(&s_generation_mux);
        busy = s_busy || s_pending;
        portEXIT_CRITICAL(&s_generation_mux);
        if (!busy) return true;
        if (timeout_ms == 0 || (xTaskGetTickCount() - start) >= timeout) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void voice_enqueue_msg(voice_msg_kind_t kind, const char *text)
{
    if (!s_queue) {
        ESP_LOGW(TAG, "voice not initialized");
        return;
    }
    voice_msg_t message = { 0 };
    message.kind = kind;
    if (text) copy_utf8_truncated(message.text, sizeof(message.text), text);
    message.generation = voice_next_generation();
    voice_set_pending(true);
    if (xQueueOverwrite(s_queue, &message) != pdPASS) {
        voice_set_pending(false);
        ESP_LOGE(TAG, "voice queue overwrite failed");
    }
}

/* 开机提示音 (非阻塞): 本地合成上行琶音, 上电 ~1s 内可闻, 兼做喇叭硬件自检 */
void voice_play_boot_chime(void)
{
    voice_enqueue_msg(VOICE_MSG_CHIME, NULL);
}

/* 1kHz 硬件哑测音: 走 voice_task 队列, 永不阻塞调用方 */
void voice_test_tone(void)
{
    voice_enqueue_msg(VOICE_MSG_TEST_TONE, NULL);
}
