#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize MQTT client and begin connecting to the broker.
/// Connection proceeds in the background; publishes are silently
/// skipped if not yet connected.
esp_err_t mqtt_reporter_init(void);

/// Publish a prediction result. Non-blocking; skips if not connected.
void mqtt_reporter_publish_result(const char *status, float score, int time_ms);

/// Publish an error event. Non-blocking; rate-limited to once per 5 seconds.
void mqtt_reporter_publish_error(const char *message);

/// True if currently connected to the MQTT broker.
bool mqtt_reporter_is_connected(void);

/// Publish sleep/wake mode status. Non-blocking; skips if not connected.
void mqtt_reporter_publish_sleep(bool is_sleeping);

#ifdef __cplusplus
}
#endif
