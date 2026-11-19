#include "bemfa.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "app_config.h"

static const char *TAG = "bemfa";

#define BIT_MQTT_UP BIT0

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t s_evt = NULL;
static volatile bool s_up = false;
static bemfa_cmd_cb_t s_cmd_cb = NULL;

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_up = true;
        esp_mqtt_client_subscribe(e->client, app_config_get()->bemfa_topic, 0);
        ESP_LOGI(TAG, "connected, subscribe \"%s\"", app_config_get()->bemfa_topic);
        if (s_evt) {
            xEventGroupSetBits(s_evt, BIT_MQTT_UP);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_up = false;
        ESP_LOGW(TAG, "disconnected");
        break;

    case MQTT_EVENT_DATA:
        /*
         * 大载荷会被拆成多个事件。命令都很短，只处理「一次收全」的情况，
         * 分片直接忽略，避免把半条命令当完整命令解析。
         */
        if (e->current_data_offset == 0 && e->data_len == e->total_data_len) {
            if (s_cmd_cb) {
                s_cmd_cb(e->data, e->data_len);
            }
        }
        break;

    default:
        break;
    }
}

esp_err_t bemfa_start(bemfa_cmd_cb_t on_cmd)
{
    s_cmd_cb = on_cmd;
    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const app_config_t *cfg = app_config_get();

    const esp_mqtt_client_config_t cfg_mqtt = {
        .broker.address.uri = "mqtt://bemfa.com:9501",
        .credentials = {
            .client_id = cfg->bemfa_uid,
            .username  = cfg->bemfa_uid,
            .authentication.password = cfg->bemfa_uid,
        },
        .session.keepalive = CONFIG_APP_MQTT_KEEPALIVE_S,
    };

    s_client = esp_mqtt_client_init(&cfg_mqtt);
    if (s_client == NULL) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

bool bemfa_wait_connected(uint32_t timeout_ms)
{
    if (s_evt == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_MQTT_UP, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_MQTT_UP) != 0;
}

bool bemfa_connected(void)
{
    return s_up;
}

void bemfa_publish(const char *payload)
{
    if (s_client == NULL || !s_up) {
        return;
    }
    esp_mqtt_client_publish(s_client, app_config_get()->bemfa_topic, payload, 0, 0, 0);
}
