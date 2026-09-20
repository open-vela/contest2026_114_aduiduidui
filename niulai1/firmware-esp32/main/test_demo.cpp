/**
 * test_demo.cpp — 测试演示实现
 * ==============================
 * LV_1 → LV_7 形态循环播放, 每 1.5s 切换
 */

#include "test_demo.h"
#include "xp_system.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TestDemo";

void test_fox_cycle_demo(void) {
    ESP_LOGI(TAG, "Fox cycle demo: LV_1 -> LV_7, 1.5s/step");

    int lv = 1;
    uint32_t last = lv_tick_get();

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));

        if (lv_tick_get() - last > 1500) {
            xp_set_level(lv);
            ESP_LOGI(TAG, "Fox Lv.%d", lv);
            lv++;
            if (lv > 7) lv = 1;
            last = lv_tick_get();
        }
    }
}
