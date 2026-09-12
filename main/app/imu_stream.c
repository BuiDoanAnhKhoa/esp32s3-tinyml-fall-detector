#include "imu_stream.h"
#include "sdkconfig.h"

#if CONFIG_MPU_SERIAL_STREAM

#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if CONFIG_ESP_CONSOLE_UART_BAUDRATE < 230400
#error "MPU serial streaming requires UART console baud >= 230400; use 460800"
#endif

static QueueHandle_t stream_queue;

static void stream_task(void *arg) {
    (void)arg;
    imu_sample_t sample;
    for (;;) {
        if (xQueueReceive(stream_queue, &sample, portMAX_DELAY) == pdTRUE) {
            // One stdio call keeps each record together with other console logs.
            // Under 160 bytes/sample at the driver's configured sensor ranges:
            // under 160 kbit/s including UART 8N1 framing at 100 Hz.
            printf("MPU1,%" PRIu32 ",%" PRId64 ",%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                   sample.sequence, sample.timestamp_us,
                   (double)sample.acc[0], (double)sample.acc[1], (double)sample.acc[2],
                   (double)sample.gyro[0], (double)sample.gyro[1], (double)sample.gyro[2]);
            fflush(stdout);
        }
    }
}

esp_err_t imu_stream_start(void) {
    if (stream_queue) return ESP_ERR_INVALID_STATE;
    stream_queue = xQueueCreate(64, sizeof(imu_sample_t));
    if (!stream_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(stream_task, "imu_stream", 4096, NULL, 3, NULL, 0) != pdPASS) {
        vQueueDelete(stream_queue);
        stream_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void imu_stream_submit(const imu_sample_t *sample) {
    if (stream_queue) (void)xQueueSend(stream_queue, sample, 0);
}

#else

esp_err_t imu_stream_start(void) { return ESP_OK; }
void imu_stream_submit(const imu_sample_t *sample) { (void)sample; }

#endif
