#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "fall_detection.h"
#include "fall_indicator.h"
#include "imu_sample.h"
#include "imu_stream.h"
#include "mqtt_reporter.h"
#include "mpu6050.h"
#include "sleep_button.h"
#include "wifi_station.h"
#include "sdkconfig.h"

#if CONFIG_FREERTOS_UNICORE
#error "Fall detection requires both ESP32-S3 cores"
#endif

_Static_assert(CONFIG_FALL_RGB_GPIO != CONFIG_I2C_MASTER_SDA &&
               CONFIG_FALL_RGB_GPIO != CONFIG_I2C_MASTER_SCL,
               "RGB pin must not overlap the MPU I2C pins");

_Static_assert(CONFIG_FALL_SLEEP_BUTTON_GPIO != CONFIG_I2C_MASTER_SDA &&
               CONFIG_FALL_SLEEP_BUTTON_GPIO != CONFIG_I2C_MASTER_SCL &&
               CONFIG_FALL_SLEEP_BUTTON_GPIO != CONFIG_FALL_RGB_GPIO,
               "Sleep button pin must not overlap I2C or RGB pins");

static mpu6050_handle_t mpu;
static QueueHandle_t sample_queue;
static QueueHandle_t state_queue;

static void imu_task(void *arg) {
    ESP_LOGI("APP", "Sampling started on Core 0 at 100 Hz");
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(10);
    configASSERT(period > 0);
    uint32_t sequence = 0;
    int64_t last_read_error_log = -5000000;
    int64_t last_queue_error_log = -5000000;

    for (;;) {
        vTaskDelayUntil(&last_wake, period);
        if (sleep_button_is_sleeping()) {
            continue;
        }
        imu_sample_t sample = {
            .timestamp_us = esp_timer_get_time(),
            .sequence = sequence++,
        };
        mpu6050_data_t motion;
        const esp_err_t read_error = mpu6050_read_motion(&mpu, &motion);
        if (read_error != ESP_OK) {
            if (sample.timestamp_us - last_read_error_log >= 5000000) {
                ESP_LOGE("APP", "MPU read failed: %s", esp_err_to_name(read_error));
                last_read_error_log = sample.timestamp_us;
            }
            continue;
        }

        sample.acc[0] = motion.acc_x;
        sample.acc[1] = motion.acc_y;
        sample.acc[2] = motion.acc_z;
        sample.gyro[0] = motion.gyr_x;
        sample.gyro[1] = motion.gyr_y;
        sample.gyro[2] = motion.gyr_z;
        imu_stream_submit(&sample);
        // Never wait for inference. A full queue drops this sample; its missing
        // sequence number makes Core 1 discard the interrupted model window.
        if (xQueueSend(sample_queue, &sample, 0) != pdTRUE &&
            sample.timestamp_us - last_queue_error_log >= 5000000) {
            ESP_LOGW("APP", "Sample queue full; Core 1 is not keeping up or has stopped");
            last_queue_error_log = sample.timestamp_us;
        }
    }
}

void app_main(void) {
    ESP_LOGI("APP", "Starting fall detector: SDA=%d, SCL=%d, RGB=%d",
             CONFIG_I2C_MASTER_SDA, CONFIG_I2C_MASTER_SCL, CONFIG_FALL_RGB_GPIO);

    // NVS is required by WiFi for calibration data storage.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Start WiFi connection in the background.
    // MQTT connects once WiFi obtains an IP; both retry automatically.
    // Failures here are non-fatal: fall detection works without network.
    if (wifi_station_init() == ESP_OK) {
        if (wifi_station_wait_connected(15000) == ESP_OK) {
            ESP_LOGI("APP", "WiFi connected");
        } else {
            ESP_LOGW("APP", "WiFi not connected after 15 s; continuing without network");
        }
    } else {
        ESP_LOGW("APP", "WiFi init failed; continuing without network");
    }
    if (mqtt_reporter_init() != ESP_OK) {
        ESP_LOGW("APP", "MQTT init failed; continuing without MQTT");
    }

    state_queue = xQueueCreate(1, sizeof(fall_state_t));
    ESP_ERROR_CHECK(state_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(fall_indicator_start(state_queue));
    ESP_LOGI("APP", "RGB ready (blue). Initializing MPU...");

    sample_queue = xQueueCreate(256, sizeof(imu_sample_t));
    esp_err_t err = sample_queue ? mpu6050_init(&mpu,
        CONFIG_I2C_MASTER_SDA, CONFIG_I2C_MASTER_SCL,
        CONFIG_I2C_MASTER_FREQUENCY) : ESP_ERR_NO_MEM;


    if (err != ESP_OK) {
        ESP_LOGE("APP", "%s failed: %s",
                 sample_queue ? "MPU initialization" : "Sample queue allocation", esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        ESP_LOGI("APP", "MPU ready. Starting model task on Core 1...");
        err = fall_detection_start(sample_queue, state_queue);
        if (err != ESP_OK) ESP_LOGE("APP", "Cannot start model task: %s", esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        const esp_err_t stream_error = imu_stream_start();
        if (stream_error != ESP_OK) {
            ESP_LOGW("APP", "MPU stream unavailable: %s", esp_err_to_name(stream_error));
        }
    }
    if (err == ESP_OK && xTaskCreatePinnedToCore(
            imu_task, "imu_task", 4096, NULL, 12, NULL, 0) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE("APP", "Cannot start sampling task: not enough internal memory");
    }
    if (err == ESP_OK) {
        esp_err_t btn_err = sleep_button_init(state_queue, sample_queue, &mpu);
        if (btn_err != ESP_OK) {
            ESP_LOGW("APP", "Sleep button unavailable: %s", esp_err_to_name(btn_err));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE("APP", "Startup stopped; LED is amber");
        const fall_state_t error = FALL_STATE_ERROR;
        xQueueOverwrite(state_queue, &error);
    }
}
