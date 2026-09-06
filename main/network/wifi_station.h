#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize WiFi in station mode and begin connecting.
/// Requires NVS, netif, and default event loop to be initialized first.
/// Returns immediately; connection proceeds in the background.
esp_err_t wifi_station_init(void);

/// Block until WiFi obtains an IP address or timeout_ms elapses.
/// Returns ESP_OK if connected, ESP_ERR_TIMEOUT otherwise.
esp_err_t wifi_station_wait_connected(int timeout_ms);

/// Non-blocking check: true if WiFi currently has an IP address.
bool wifi_station_is_connected(void);

#ifdef __cplusplus
}
#endif
