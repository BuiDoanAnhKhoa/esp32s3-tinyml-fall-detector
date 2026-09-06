#include "fall_detection.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "fall_indicator.h"
#include "model_runtime.h"
#include "mqtt_reporter.h"
#include "sdkconfig.h"

namespace {
constexpr int64_t kMaxQueuedAgeUs = 2000000;
constexpr int64_t kPredictionBudgetUs = 1000000;
QueueHandle_t sample_queue;
QueueHandle_t state_queue;
fall_detection::ModelInputWindow window;
fall_detection::ModelRuntime model;

// Faults can happen at 100 Hz. Log the first one, then at most once per five
// seconds so diagnostics do not become another source of timing problems.
bool LogFaultNow() {
    static int64_t last_log = -5000000;
    const int64_t now = esp_timer_get_time();
    if (now - last_log < 5000000) return false;
    last_log = now;
    return true;
}

void Report(fall_state_t state) {
    xQueueOverwrite(state_queue, &state);
}

void InferenceTask(void *) {
    const size_t arena_size = CONFIG_FALL_MODEL_ARENA_KB * 1024;
    ESP_LOGI("FALL", "Model starting: PSRAM free=%u, largest block=%u, needed=%u bytes",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)), unsigned(arena_size));
    auto *arena = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16, arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arena) {
        ESP_LOGE("FALL", "Cannot allocate model memory in PSRAM; LED is amber");
        Report(FALL_STATE_ERROR);
        vTaskDelete(nullptr);
        return;
    }
    if (!model.Initialize(arena, arena_size)) {
        ESP_LOGE("FALL", "Model setup failed after memory allocation: %s; LED is amber", model.LastError());
        Report(FALL_STATE_ERROR);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI("FALL", "Model ready; working memory=%u bytes. Collecting 200 samples...", unsigned(model.ArenaUsedBytes()));
    xQueueReset(sample_queue);
    Report(FALL_STATE_WARMUP);
    bool warming_up = true;
    fall_state_t collection_state = FALL_STATE_WARMUP;
    uint32_t previous_sequence = 0;
    int64_t previous_timestamp = 0;

    for (;;) {
        imu_sample_t sample;
        if (xQueueReceive(sample_queue, &sample, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (LogFaultNow()) ESP_LOGW("FALL", "No sensor samples for 100 ms; check MPU read errors");
            window.Reset();
            warming_up = true;
            collection_state = FALL_STATE_ERROR;
            Report(FALL_STATE_ERROR);
            continue;
        }
        if (esp_timer_get_time() - sample.timestamp_us > kMaxQueuedAgeUs) {
            if (LogFaultNow()) ESP_LOGW("FALL", "Queued samples are over 2 seconds old; restarting window");
            window.Reset();
            warming_up = true;
            collection_state = FALL_STATE_ERROR;
            Report(FALL_STATE_ERROR);
            continue;
        }
        const auto update = window.Push(sample);
        const uint32_t sequence_step = sample.sequence - previous_sequence;
        const int64_t interval_us = sample.timestamp_us - previous_timestamp;
        previous_sequence = sample.sequence;
        previous_timestamp = sample.timestamp_us;
        if (update == fall_detection::WindowUpdate::Invalid) {
            if (LogFaultNow()) ESP_LOGW("FALL", "Invalid sensor values (NaN/Inf); restarting window");
            warming_up = true;
            collection_state = FALL_STATE_ERROR;
            Report(FALL_STATE_ERROR);
            continue;
        }
        if (update == fall_detection::WindowUpdate::Restarted) {
            if (LogFaultNow()) ESP_LOGW("FALL", "Sample gap: step=%lu, interval=%lld us (expected step=1, 5000..15000 us)",
                                       (unsigned long)sequence_step, (long long)interval_us);
            warming_up = true;
            collection_state = FALL_STATE_ERROR;
        }
        if (update != fall_detection::WindowUpdate::Ready) {
            if (warming_up) Report(collection_state);
            continue;
        }

        float score;
        const int64_t started = esp_timer_get_time();
        const bool valid = model.Predict(window, &score);
        const int64_t elapsed = esp_timer_get_time() - started;
        if (!valid || elapsed > kPredictionBudgetUs) {
            if (LogFaultNow()) {
                if (!valid) ESP_LOGE("FALL", "Prediction failed: %s", model.LastError());
                else ESP_LOGE("FALL", "Prediction too slow: %lld ms (limit 1000 ms)", (long long)(elapsed / 1000));
            }
            mqtt_reporter_publish_error(!valid ? "Prediction failed" : "Prediction too slow");
            Report(FALL_STATE_ERROR);
            window.Reset();
            xQueueReset(sample_queue);
            warming_up = true;
            collection_state = FALL_STATE_ERROR;
        } else {
            warming_up = false;
            collection_state = FALL_STATE_WARMUP;
            Report(score >= kFallThreshold ? FALL_STATE_DETECTED : FALL_STATE_NORMAL);
            ESP_LOGI("FALL", "%s: score=%.4f (threshold=%.2f), time=%lld ms",
                     score >= kFallThreshold ? "FALL" : "NORMAL", double(score), double(kFallThreshold), (long long)(elapsed / 1000));
            mqtt_reporter_publish_result(
                score >= kFallThreshold ? "FALL" : "NORMAL",
                score, (int)(elapsed / 1000));
        }
        // Let the Core 1 idle task run, including while draining queued samples.
        vTaskDelay(1);
    }
}
}  // namespace

extern "C" esp_err_t fall_detection_start(QueueHandle_t samples, QueueHandle_t states) {
    if (!samples || !states || sample_queue) return ESP_ERR_INVALID_ARG;
    sample_queue = samples;
    state_queue = states;
    return xTaskCreatePinnedToCore(InferenceTask, "fall_model", 12288, nullptr, 5, nullptr, 1) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
