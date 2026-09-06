#include "mqtt_reporter.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "MQTT";
static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        s_connected = true;
        esp_mqtt_client_publish(s_client, "fall-detector/online",
                                "{\"status\":\"online\"}", 0, 0, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Transport error: esp_tls=0x%x, errno=%d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

esp_err_t mqtt_reporter_init(void) {
    char uri[64];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d",
             CONFIG_FALL_MQTT_BROKER_IP, CONFIG_FALL_MQTT_BROKER_PORT);

    const esp_mqtt_client_config_t cfg = {
        .broker = {
            .address = {
                .uri = uri,
            },
        },
        .credentials = {
            .client_id = "fall-detector-esp32",
        },
        .session = {
            .last_will = {
                .topic = "fall-detector/online",
                .msg = "{\"status\":\"offline\"}",
                .msg_len = 0,
                .qos = 1,
                .retain = 1,
            },
        },
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Client started, broker=%s", uri);
    }
    return err;
}

void mqtt_reporter_publish_result(const char *status, float score, int time_ms) {
    if (!s_connected) return;
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"%s\",\"score\":%.4f,\"time_ms\":%d}",
             status, (double)score, time_ms);
    esp_mqtt_client_publish(s_client, "fall-detector/result", payload, 0, 1, 0);
}

void mqtt_reporter_publish_error(const char *message) {
    if (!s_connected) return;
    // Rate limit: at most once per 5 seconds.
    static int64_t last_publish = -5000000;
    int64_t now = esp_timer_get_time();
    if (now - last_publish < 5000000) return;
    last_publish = now;

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"error\":\"%s\"}", message);
    esp_mqtt_client_publish(s_client, "fall-detector/error", payload, 0, 1, 0);
}

bool mqtt_reporter_is_connected(void) {
    return s_connected;
}
