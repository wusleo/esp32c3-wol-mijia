#include "app_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cfg";

/* 单独一个命名空间，避免和 WiFi/IP 协议栈自己的 NVS 数据混在一起 */
static const char *NVS_NS = "wolcfg";

/* 「下次启动强制进配网」标志，NVS 里存 u8 */
#define NVS_KEY_FORCE_PROV "forcprov"

static app_config_t s_cfg;
static bool s_from_nvs = false;
static bool s_loaded = false;

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, cap);
}

/* Kconfig 里的值作为出厂默认。开源版本默认全是空串，因此必然进配网 */
static void fill_from_kconfig(app_config_t *c)
{
    memset(c, 0, sizeof(*c));

    copy_str(c->wifi_ssid, sizeof(c->wifi_ssid), CONFIG_APP_WIFI_SSID);
    copy_str(c->wifi_password, sizeof(c->wifi_password), CONFIG_APP_WIFI_PASSWORD);
    copy_str(c->bemfa_uid, sizeof(c->bemfa_uid), CONFIG_APP_BEMFA_UID);
    copy_str(c->bemfa_topic, sizeof(c->bemfa_topic), CONFIG_APP_BEMFA_TOPIC);
    copy_str(c->target_mac, sizeof(c->target_mac), CONFIG_APP_TARGET_MAC);
    copy_str(c->broadcast, sizeof(c->broadcast), CONFIG_APP_TARGET_BROADCAST);

    c->magic_port = CONFIG_APP_MAGIC_PORT;

    /* 广播地址留空时由 wol.c 从 IP + 掩码自动推导，比写死 255.255.255.255 可靠 */
    if (c->broadcast[0] == '\0') {
        copy_str(c->broadcast, sizeof(c->broadcast), "auto");
    }
    if (c->magic_port <= 0 || c->magic_port > 65535) {
        c->magic_port = 9;
    }
}

/* 返回是否读到了非空值，用于判断 NVS 里到底有没有真配置 */
static bool read_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;

    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        return false;
    }
    return out[0] != '\0';
}

esp_err_t app_config_load(void)
{
    fill_from_kconfig(&s_cfg);
    s_from_nvs = false;
    s_loaded = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no saved config (%s), falling back to menuconfig",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    bool any = false;

    any |= read_str(h, "ssid",  s_cfg.wifi_ssid,     sizeof(s_cfg.wifi_ssid));
    any |= read_str(h, "pass",  s_cfg.wifi_password, sizeof(s_cfg.wifi_password));
    any |= read_str(h, "uid",   s_cfg.bemfa_uid,     sizeof(s_cfg.bemfa_uid));
    any |= read_str(h, "topic", s_cfg.bemfa_topic,   sizeof(s_cfg.bemfa_topic));
    any |= read_str(h, "mac",   s_cfg.target_mac,    sizeof(s_cfg.target_mac));

    nvs_close(h);

    s_from_nvs = any;

    if (any) {
        ESP_LOGI(TAG, "loaded config from nvs (ssid=\"%s\", mac=%s)",
                 s_cfg.wifi_ssid, s_cfg.target_mac);
    } else {
        ESP_LOGW(TAG, "nvs namespace exists but holds no usable value");
    }

    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    if (!s_loaded) {
        /* 防御：调用方没先 load 也不会拿到全 0 的指针解引用 */
        fill_from_kconfig(&s_cfg);
        s_loaded = true;
    }
    return &s_cfg;
}

bool app_config_from_nvs(void)
{
    return s_from_nvs;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, "ssid", cfg->wifi_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", cfg->wifi_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "uid", cfg->bemfa_uid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "topic", cfg->bemfa_topic);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "mac", cfg->target_mac);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err == ESP_OK) {
        s_cfg = *cfg;
        s_from_nvs = true;
        s_loaded = true;
        ESP_LOGI(TAG, "config saved (ssid=\"%s\", mac=%s)",
                 s_cfg.wifi_ssid, s_cfg.target_mac);
    } else {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t app_config_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        s_from_nvs = false;
        fill_from_kconfig(&s_cfg);
        ESP_LOGW(TAG, "config erased, reverted to menuconfig defaults");
    }

    return err;
}

bool app_config_force_prov(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_FORCE_PROV, &v);
    nvs_close(h);

    return err == ESP_OK && v != 0;
}

esp_err_t app_config_set_force_prov(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(h, NVS_KEY_FORCE_PROV, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGW(TAG, "force provisioning -> %s", on ? "on" : "off");
    return err;
}

bool app_config_mac_valid(const char *mac)
{
    if (mac == NULL) {
        return false;
    }

    unsigned v[6] = { 0 };

    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        if (sscanf(mac, "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
            return false;
        }
    }

    bool all_zero = true;
    bool all_ff = true;

    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) {
            return false;
        }
        if (v[i] != 0x00) {
            all_zero = false;
        }
        if (v[i] != 0xFF) {
            all_ff = false;
        }
    }

    return !all_zero && !all_ff;
}

bool app_config_complete(void)
{
    return s_cfg.wifi_ssid[0] != '\0' &&
           s_cfg.bemfa_uid[0] != '\0' &&
           s_cfg.bemfa_topic[0] != '\0' &&
           app_config_mac_valid(s_cfg.target_mac);
}
