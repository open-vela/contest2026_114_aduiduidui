#include "reading_manager.h"

#include "cam_stream.h"
#include "ocr_reader.h"
#include "ui_layout.h"
#include "voice.h"
#include "fox_companion.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdint.h>
#include <limits.h>
#include <string.h>

static const char *TAG = "Reading";
#define READING_TEXT_MAX 2048
#define READING_BREAK_SECONDS (25U * 60U)
#define READING_WAIT_SLICE_MS 100U

typedef struct {
    char text[READING_TEXT_MAX];
    uint32_t session;
} reading_job_t;

static QueueHandle_t s_jobs;
static volatile uint32_t s_session;
static volatile uint32_t s_active_seconds;
static portMUX_TYPE s_session_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t next_session(void)
{
    uint32_t value;
    portENTER_CRITICAL(&s_session_lock);
    value = ++s_session;
    if (value == 0) value = ++s_session;
    portEXIT_CRITICAL(&s_session_lock);
    return value;
}

static size_t utf8_copy(char *dst, size_t cap, const char *src)
{
    if (cap < 2 || !src) return 0;
    size_t n = strnlen(src, cap - 1);
    if (src[n] != 0) while (n > 0 && (((uint8_t)src[n] & 0xc0U) == 0x80U)) n--;
    memcpy(dst, src, n); dst[n] = 0; return n;
}

static bool is_break_char(const uint8_t *p, size_t remaining)
{
    if (*p == '\n' || *p == '.' || *p == '!' || *p == '?' || *p == ';' || *p == ':') return true;
    if (remaining >= 3 && p[0] == 0xe3 && p[1] == 0x80 &&
        (p[2] == 0x82 || p[2] == 0x81 || p[2] == 0x80 || p[2] == 0x84)) return true;
    if (remaining >= 3 && p[0] == 0xef && p[1] == 0xbc &&
        (p[2] == 0x81 || p[2] == 0x8c || p[2] == 0x9b || p[2] == 0x9f)) return true;
    return false;
}

static size_t next_sentence(const char *text)
{
    size_t total = strlen(text);
    if (total <= 240) return total;
    size_t limit = 240, i = 0, last = 0;
    while (i < total && i < limit) {
        const uint8_t *p = (const uint8_t *)text + i;
        size_t n = 1;
        if ((p[0] & 0xe0U) == 0xc0U) n = 2;
        else if ((p[0] & 0xf0U) == 0xe0U) n = 3;
        else if ((p[0] & 0xf8U) == 0xf0U) n = 4;
        if (i + n > limit || i + n > total) break;
        if (is_break_char(p, total - i)) last = i + n;
        i += n;
    }
    return last >= limit / 2 ? last : limit;
}

static bool session_current(uint32_t session)
{
    bool current;
    portENTER_CRITICAL(&s_session_lock);
    current = session == s_session;
    portEXIT_CRITICAL(&s_session_lock);
    return current;
}

static bool wait_voice(uint32_t session, bool count_reading)
{
    uint32_t elapsed_ms = 0;
    while (voice_wait_idle(0) == false) {
        if (!session_current(session)) return false;
        vTaskDelay(pdMS_TO_TICKS(READING_WAIT_SLICE_MS));
        elapsed_ms += READING_WAIT_SLICE_MS;
        if (count_reading && elapsed_ms >= 1000) {
            uint32_t seconds = elapsed_ms / 1000;
            elapsed_ms %= 1000;
            if (UINT32_MAX - s_active_seconds < seconds) s_active_seconds = UINT32_MAX;
            else s_active_seconds += seconds;
            if (s_active_seconds >= READING_BREAK_SECONDS) return false;
        }
    }
    return session_current(session);
}

static void break_guidance(uint32_t session)
{
    ui_set_reading_break(true);
    fox_handle_event(FOX_EVENT_BREAK);
    voice_interrupt();
    voice_speak_async("你已经阅读25分钟啦，我们休息一下。");
    (void)wait_voice(session, false);
    if (!session_current(session)) { ui_set_reading_break(false); return; }
    voice_speak_async("请眺望远处二十秒，然后眨眨眼。");
    (void)wait_voice(session, false);
    if (!session_current(session)) { ui_set_reading_break(false); return; }
    voice_speak_async("跟着屏幕做眼保健操，完成后再继续阅读。");
    (void)wait_voice(session, false);
    s_active_seconds = 0;
    ui_set_reading_break(false);
    fox_handle_event(FOX_EVENT_BREAK_DONE);
}

static void reading_task(void *arg)
{
    (void)arg;
    reading_job_t job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (!session_current(job.session)) continue;
        s_active_seconds = 0;
        fox_handle_event(FOX_EVENT_READING_START);
        const char *cursor = job.text;
        while (*cursor && session_current(job.session)) {
            size_t bytes = next_sentence(cursor);
            if (bytes == 0) break;
            char sentence[241];
            memcpy(sentence, cursor, bytes); sentence[bytes] = 0;
            voice_speak_async(sentence);
            if (!wait_voice(job.session, true)) {
                if (s_active_seconds >= READING_BREAK_SECONDS && session_current(job.session))
                    break_guidance(job.session);
                break;
            }
            cursor += bytes;
        }
        if (session_current(job.session)) fox_handle_event(FOX_EVENT_IDLE);
    }
}

bool reading_manager_init(void)
{
    if (s_jobs) return true;
    s_jobs = xQueueCreate(1, sizeof(reading_job_t));
    if (!s_jobs) return false;
    s_session = 1;
    if (xTaskCreate(reading_task, "reading", 6144, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(s_jobs); s_jobs = NULL; return false;
    }
    ESP_LOGI(TAG, "reading manager ready (OCR + 25min reminder)");
    return true;
}

bool reading_start_text(const char *text)
{
    if (!s_jobs || !text || !text[0]) return false;
    reading_job_t job = { 0 };
    job.session = next_session();
    if (!utf8_copy(job.text, sizeof(job.text), text)) return false;
    voice_interrupt();
    if (xQueueOverwrite(s_jobs, &job) != pdPASS) return false;
    return true;
}

void reading_stop(void)
{
    if (!s_jobs) return;
    (void)next_session();
    (void)xQueueReset(s_jobs);
    voice_interrupt();
    s_active_seconds = 0;
    ui_set_reading_break(false);
    fox_handle_event(FOX_EVENT_IDLE);
}

bool reading_capture_and_start(void)
{
    if (!s_jobs) return false;
    reading_stop();
    cam_stream_pause(true);
    vTaskDelay(pdMS_TO_TICKS(150)); /* 让抓图任务归还可能正持有的帧缓冲 */
    camera_fb_t *fb = ui_capture_frame();
    bool ok = false;
    char text[READING_TEXT_MAX];
    if (fb) {
        fox_handle_event(FOX_EVENT_QUESTION);
        ok = ocr_request_text(fb, text, sizeof(text));
        ui_frame_return(fb);
    }
    cam_stream_pause(false);
    if (!ok) {
        fox_handle_event(FOX_EVENT_ANSWER_ERROR);
        ESP_LOGW(TAG, "OCR capture/request failed");
        return false;
    }
    fox_handle_event(FOX_EVENT_OCR_SUCCESS);
    ESP_LOGI(TAG, "OCR text length=%u", (unsigned)strlen(text));
    return reading_start_text(text);
}
