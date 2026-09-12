#include "fall_indicator.h"

#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

static led_strip_handle_t strip;

static esp_err_t show_state(fall_state_t state) {
    uint32_t red = 0, green = 0, blue = 0;
    switch (state) {
    case FALL_STATE_WARMUP: blue = 8; break;
    case FALL_STATE_DETECTED: red = CONFIG_FALL_RGB_BRIGHTNESS; break;
    case FALL_STATE_ERROR: red = 32; green = 8; break;
    case FALL_STATE_NORMAL: break;
    case FALL_STATE_SLEEP: green = 6; blue = 8; break;
    }
    esp_err_t err = led_strip_set_pixel(strip, 0, red, green, blue);
    return err == ESP_OK ? led_strip_refresh(strip) : err;
}

static void indicator_task(void *arg) {
    QueueHandle_t states = (QueueHandle_t)arg;
    fall_state_t previous = FALL_STATE_WARMUP;
    bool timed_out = false;
    for (;;) {
        fall_state_t state;
        // A stalled model must not leave a stale "normal" indication forever.
        if (xQueueReceive(states, &state, pdMS_TO_TICKS(5000)) != pdTRUE) {
            if (!timed_out) ESP_LOGW("RGB", "No detector status for 5 seconds; showing amber");
            timed_out = true;
            state = FALL_STATE_ERROR;
        } else {
            timed_out = false;
        }
        if (state != previous) {
            ESP_ERROR_CHECK(show_state(state));
            previous = state;
        }
    }
}

esp_err_t fall_indicator_start(QueueHandle_t states) {
    const led_strip_config_t config = {
        .strip_gpio_num = CONFIG_FALL_RGB_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&config, &rmt_config, &strip);
    if (err != ESP_OK) return err;
    err = show_state(FALL_STATE_WARMUP);
    if (err != ESP_OK) return err;
    return xTaskCreatePinnedToCore(indicator_task, "fall_rgb", 3072, states, 5, NULL, 0) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
