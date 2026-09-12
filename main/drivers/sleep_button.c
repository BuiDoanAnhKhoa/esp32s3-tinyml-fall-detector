#include "sleep_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fall_indicator.h"
#include "mqtt_reporter.h"
#include "sdkconfig.h"

static const char *TAG = "SLEEP_BTN";

static volatile bool s_sleeping = false;
static QueueHandle_t s_state_queue;
static QueueHandle_t s_sample_queue;
static mpu6050_handle_t *s_mpu;
static SemaphoreHandle_t s_button_sem;

static void IRAM_ATTR button_isr_handler(void *arg) {
    static int64_t last_press = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_press < 250000) return;  // 250 ms debounce
    last_press = now;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_button_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_button_sem, portMAX_DELAY);

        s_sleeping = !s_sleeping;

        if (s_sleeping) {
            ESP_LOGI(TAG, "Entering sleep mode");

            // Let the IMU task see the flag and stop reading.
            vTaskDelay(pdMS_TO_TICKS(20));

            // Drain residual samples so inference does not process stale data.
            xQueueReset(s_sample_queue);

            // Put the sensor into hardware sleep.
            if (s_mpu) mpu6050_sleep(s_mpu);

            // Steady dim cyan on the onboard LED.
            fall_state_t st = FALL_STATE_SLEEP;
            xQueueOverwrite(s_state_queue, &st);

            // Notify MQTT subscribers.
            mqtt_reporter_publish_sleep(true);
        } else {
            ESP_LOGI(TAG, "Waking up from sleep mode");

            // Wake the sensor and let it stabilise.
            if (s_mpu) mpu6050_wake(s_mpu);
            vTaskDelay(pdMS_TO_TICKS(50));

            // Clear any garbage in the queue.
            xQueueReset(s_sample_queue);

            // Show warmup blue while the window refills.
            fall_state_t st = FALL_STATE_WARMUP;
            xQueueOverwrite(s_state_queue, &st);

            // Notify MQTT subscribers.
            mqtt_reporter_publish_sleep(false);
        }
    }
}

esp_err_t sleep_button_init(QueueHandle_t state_queue,
                            QueueHandle_t sample_queue,
                            mpu6050_handle_t *mpu) {
    s_state_queue  = state_queue;
    s_sample_queue = sample_queue;
    s_mpu          = mpu;

    s_button_sem = xSemaphoreCreateBinary();
    if (!s_button_sem) return ESP_ERR_NO_MEM;

    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << CONFIG_FALL_SLEEP_BUTTON_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = gpio_isr_handler_add(CONFIG_FALL_SLEEP_BUTTON_GPIO,
                               button_isr_handler, NULL);
    if (err != ESP_OK) return err;

    if (xTaskCreatePinnedToCore(button_task, "sleep_btn", 3072,
                                NULL, 10, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sleep button on GPIO %d", CONFIG_FALL_SLEEP_BUTTON_GPIO);
    return ESP_OK;
}

bool sleep_button_is_sleeping(void) {
    return s_sleeping;
}
