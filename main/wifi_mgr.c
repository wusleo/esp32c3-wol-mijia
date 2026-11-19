#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "app_config.h"

static const char *TAG = "wifi";

#define AP_SCAN_MAX 16
#define BIT_STA_START BIT0
#define BIT_GOT_IP    BIT1

static EventGroupHandle_t s_evt = NULL;
static TaskHandle_t s_reconnect_task = NULL;
static volatile bool s_connected = false;

/*
 * 扫描结果约 16 * sizeof(wifi_ap_record_t) ≈ 1.8KB。
 * 绝对不能放栈上：事件回调所在的 sys_evt 任务栈只有约 2.3KB，
 * 早期版本就因为这个溢出直接 panic 了。
 */
static wifi_ap_record_t s_ap[AP_SCAN_MAX];

/* ------------------------------------------------------------------ */

static bool pick_best_ap(uint8_t bssid[6], uint8_t *channel, int8_t *rssi_out)
{
    wifi_scan_config_t sc = { .show_hidden = false };

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t num = AP_SCAN_MAX;
    err = esp_wifi_scan_get_ap_records(&num, s_ap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get records failed: %s", esp_err_to_name(err));
        return false;
    }

    const char *want = app_config_get()->wifi_ssid;
    int best = -1;
    int8_t best_rssi = -128;

    for (int i = 0; i < num; i++) {
        if (strcmp((const char *)s_ap[i].ssid, want) == 0 &&
            s_ap[i].rssi > best_rssi) {
            best = i;
            best_rssi = s_ap[i].rssi;
        }
    }

    if (best < 0) {
        ESP_LOGW(TAG, "scanned %u APs, none is \"%s\"", num, want);
        return false;
    }

    memcpy(bssid, s_ap[best].bssid, 6);
    *channel = s_ap[best].primary;
    *rssi_out = best_rssi;

    ESP_LOGI(TAG, "picked ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d",
             s_ap[best].primary,
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
             best_rssi);
    return true;
}

static void connect_best(void)
{
    const app_config_t *cfg = app_config_get();
    wifi_config_t wc = { 0 };

    strlcpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->wifi_password, sizeof(wc.sta.password));

    /* 密码为空说明是开放网络，此时把认证门槛降到 OPEN，否则连不上 */
    wc.sta.threshold.authmode =
        (cfg->wifi_password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

#if CONFIG_APP_WIFI_PS_MAX_MODEM
    /*
     * 省电核心参数：每隔多少个 AP beacon 才醒一次收包。
     * 等效于路由器侧的 DTIM 周期，但只影响本设备。
     */
    wc.sta.listen_interval = CONFIG_APP_WIFI_LISTEN_INTERVAL;
#endif

    uint8_t bssid[6];
    uint8_t channel = 0;
    int8_t rssi = 0;

    if (pick_best_ap(bssid, &channel, &rssi)) {
        memcpy(wc.sta.bssid, bssid, 6);
        wc.sta.bssid_set = true;
        wc.sta.channel = channel;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        return;
    }

    wifi_mgr_apply_tx_power();

    err = esp_wifi_connect();
    ESP_LOGI(TAG, "connect -> %s", esp_err_to_name(err));
}

/* 事件回调跑在 sys_evt 任务里，只做最轻的动作 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            xEventGroupSetBits(s_evt, BIT_STA_START);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
            s_connected = false;
            xEventGroupClearBits(s_evt, BIT_GOT_IP);

            if (s_reconnect_task != NULL) {
                /* 不在回调里扫描，交给专用任务 */
                xTaskNotifyGive(s_reconnect_task);
            }

            if (e->reason == WIFI_REASON_AUTH_EXPIRE ||
                e->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                /*
                 * 这两个原因在高功率失真时非常典型：
                 * 板子以为自己发出去了，AP 却收不到，于是反复超时。
                 * 提示一下方向，省得新手以为是密码错了。
                 */
                ESP_LOGW(TAG, "disconnected reason=%d (auth/handshake timeout) -- "
                              "check password, or lower CONFIG_APP_WIFI_TX_POWER_QDBM",
                         e->reason);
            } else {
                ESP_LOGW(TAG, "disconnected reason=%d", e->reason);
            }
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        xEventGroupSetBits(s_evt, BIT_GOT_IP);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void reconnect_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* 已被别的路径重连成功就跳过 */
        if (s_connected) {
            continue;
        }

        ESP_LOGW(TAG, "reconnecting (with rescan)");
        connect_best();
    }
}

/* ------------------------------------------------------------------ */

esp_err_t wifi_mgr_apply_tx_power(void)
{
    esp_err_t err = esp_wifi_set_max_tx_power(CONFIG_APP_WIFI_TX_POWER_QDBM);
    ESP_LOGI(TAG, "tx power %d (=%.2f dBm) -> %s",
             CONFIG_APP_WIFI_TX_POWER_QDBM,
             CONFIG_APP_WIFI_TX_POWER_QDBM / 4.0,
             esp_err_to_name(err));
    return err;
}

esp_err_t wifi_mgr_apply_ps_mode(void)
{
#if CONFIG_APP_WIFI_PS_MAX_MODEM
    wifi_ps_type_t ps = WIFI_PS_MAX_MODEM;
#else
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
#endif
    esp_err_t err = esp_wifi_set_ps(ps);
    ESP_LOGI(TAG, "ps mode %d -> %s", (int)ps, esp_err_to_name(err));
    return err;
}

esp_err_t wifi_mgr_start(void)
{
    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* 配置由 app_config 统一管理，协议栈这层不需要再存一份 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_evt, BIT_STA_START, pdFALSE, pdTRUE, portMAX_DELAY);

    xTaskCreate(reconnect_task, "wifi_retry", 3072, NULL, 4, &s_reconnect_task);

    connect_best();

    return ESP_OK;
}

bool wifi_mgr_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_GOT_IP, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_GOT_IP) != 0;
}

bool wifi_mgr_connected(void)
{
    return s_connected;
}

bool wifi_mgr_get_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }

    snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    return true;
}
