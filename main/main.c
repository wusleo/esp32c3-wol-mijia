/*
 * ESP32-C3 远程唤醒器（巴法云 / 米家 / 小爱同学）
 *
 * 一块十几块钱的 C3 SuperMini，接在电脑旁边的 USB 口上，
 * 就能让小爱同学开口把电脑叫起来，并且可以只用一节 18650 供电跑上几个月。
 *
 * 这个工程的真正难点不在 WOL，而在于「省电」和「可维护」之间的矛盾：
 *
 *   省电要求 CPU 进 light sleep，而 light sleep 会关掉 USB-Serial/JTAG 的时钟，
 *   串口随之失效。串口一失效，既看不到日志也烧不了固件，板子等于变砖。
 *
 *   解决办法是给固件留一条后门：通过 MQTT 下发 "debug"，
 *   设备持有一把 ESP_PM_NO_LIGHT_SLEEP 锁保持清醒，串口恢复后再烧录。
 *
 * 整个链路的验证记录、踩坑清单和功耗实测，见仓库 README。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bemfa.h"
#include "app_config.h"
#include "power.h"
#include "provisioning.h"
#include "wifi_mgr.h"
#include "wol.h"

#define APP_VERSION "1.1.1"

/* Kconfig 的 bool 项在关掉时是 #undef，直接写进表达式会编译失败 */
#ifdef CONFIG_APP_ENABLE_LIGHT_SLEEP
#define FORCE_LIGHT_SLEEP_FLAG 1
#else
#define FORCE_LIGHT_SLEEP_FLAG 0
#endif

static const char *TAG = "main";

static uint8_t s_target_mac[6];

/* ------------------------------------------------------------------ */
/* 配置校验                                                            */
/* ------------------------------------------------------------------ */

/*
 * 配置不完整时不能直接开跑 —— 那样 WiFi 连不上、MQTT 连不上，
 * 串口又没有明显报错，新手只会以为买到了坏板子。
 *
 * 现在配置来自 app_config（NVS 优先，退到 menuconfig），
 * 两者都没填的情况下直接转配网模式。
 */
static bool validate_config(void)
{
    const app_config_t *cfg = app_config_get();
    bool ok = true;

    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGE(TAG, "wifi ssid is empty");
        ok = false;
    }

    if (cfg->bemfa_uid[0] == '\0') {
        ESP_LOGE(TAG, "bemfa uid is empty");
        ok = false;
    }

    if (cfg->bemfa_topic[0] == '\0') {
        ESP_LOGE(TAG, "bemfa topic is empty");
        ok = false;
    }

    if (!wol_parse_mac(cfg->target_mac, s_target_mac) ||
        !wol_mac_is_valid(s_target_mac)) {
        ESP_LOGE(TAG, "target mac \"%s\" is not a usable MAC", cfg->target_mac);
        ESP_LOGE(TAG, "use the wired NIC MAC, format AA:BB:CC:DD:EE:FF");
        ok = false;
    }

    return ok;
}

/* ------------------------------------------------------------------ */
/* MQTT 指令                                                           */
/* ------------------------------------------------------------------ */

/*
 * 巴法云会把同一 topic 上的消息回送给所有订阅者，包括发布者自己。
 * 如果收到命令就回一条，那条回复又会被自己收到并当成新命令，
 * 于是形成「收到 -> 回复 -> 再收到」的自激循环，既刷屏又耗电。
 *
 * 所有回复都带固定前缀，收到这些前缀的消息直接忽略。
 */
static const char *const OWN_ECHO_PREFIXES[] = {
    "wol:", "debug:", "status:", "cmd:", "ack:", "boot:", "ready:", "alive:",
};

static bool is_own_echo(const char *data, int len)
{
    for (size_t i = 0; i < sizeof(OWN_ECHO_PREFIXES) / sizeof(OWN_ECHO_PREFIXES[0]); i++) {
        size_t n = strlen(OWN_ECHO_PREFIXES[i]);
        if ((size_t)len >= n && memcmp(data, OWN_ECHO_PREFIXES[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* 去掉首尾空白和引号，再忽略大小写比较，容忍平台额外的包装字符 */
static bool payload_is(const char *data, int len, const char *expected)
{
    while (len > 0 && (data[0] == ' ' || data[0] == '\r' || data[0] == '\n' ||
                       data[0] == '\t' || data[0] == '"')) {
        data++;
        len--;
    }
    while (len > 0) {
        char c = data[len - 1];
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t' && c != '"') {
            break;
        }
        len--;
    }

    size_t n = strlen(expected);
    if ((size_t)len != n) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        char a = data[i];
        char b = expected[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

static void on_mqtt_cmd(const char *data, int len)
{
    if (is_own_echo(data, len)) {
        ESP_LOGD(TAG, "ignore own echo \"%.*s\"", len, data);
        return;
    }

    ESP_LOGI(TAG, "cmd \"%.*s\"", len, data);

    /* 把收到的原始字节回显成 hex，方便在没有串口时确认平台到底发了什么 */
    {
        char hex[3 * 32 + 8];
        int p = snprintf(hex, sizeof(hex), "ack:");
        for (int i = 0; i < len && i < 32 && p < (int)sizeof(hex) - 4; i++) {
            p += snprintf(&hex[p], sizeof(hex) - (size_t)p, "%02x", (unsigned char)data[i]);
        }
        bemfa_publish(hex);
    }

    if (payload_is(data, len, "on")) {
        esp_err_t err = wol_send(s_target_mac,
                                 app_config_get()->broadcast,
                                 app_config_get()->magic_port);
        bemfa_publish(err == ESP_OK ? "wol:sent" : "wol:failed");

    } else if (payload_is(data, len, "off")) {
        /* 关机不在本工程能力范围内，明确回一条，避免用户以为坏了 */
        bemfa_publish("wol:off-unsupported");

    } else if (payload_is(data, len, "debug")) {
        power_awake_begin(CONFIG_APP_AWAKE_WINDOW_S);
        bemfa_publish("debug:awake");

    } else if (payload_is(data, len, "sleep")) {
        power_awake_end();
        bemfa_publish("debug:sleeping");

    } else if (payload_is(data, len, "status")) {
        bemfa_publish(power_light_sleep_on() ? "status:sleep-on" : "status:sleep-off");

    } else {
        bemfa_publish("cmd:unknown");
    }
}

/* ------------------------------------------------------------------ */
/* 后勤任务                                                            */
/* ------------------------------------------------------------------ */

static void housekeeping_task(void *arg)
{
    uint32_t elapsed_s = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* 调试窗口到期后必须有人把锁还回去 */
        power_tick();

        elapsed_s++;

#if CONFIG_APP_STATUS_REPORT
        if (elapsed_s % CONFIG_APP_STATUS_INTERVAL_S == 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "alive:%us,sleep:%d",
                     (unsigned)(esp_timer_get_time() / 1000000),
                     power_light_sleep_on() ? 1 : 0);
            bemfa_publish(buf);
        }
#endif
    }
}

/* ------------------------------------------------------------------ */

static bool init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    /* 全新芯片或换过分区表都会走到这里，擦一次即可 */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing nvs (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err == ESP_OK;
}

void app_main(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " ESP32-C3 remote wake  v%s", APP_VERSION);
    ESP_LOGI(TAG, " chip rev %d.%d, %d core(s)",
             ci.revision / 100, ci.revision % 100, ci.cores);
    ESP_LOGI(TAG, "==============================================");

    if (!init_nvs()) {
        ESP_LOGE(TAG, "nvs init failed");
        return;
    }

    /* NVS 里的配网页配置优先，读不到才退到 menuconfig 的出厂值 */
    app_config_load();

    if (!validate_config() || app_config_force_prov()) {
#if CONFIG_APP_PROVISIONING
        /*
         * 两种情况会进这里：
         *   1. 配置不完整（全新板子 / 刚擦过 NVS）
         *   2. 上次启动连续拿不到 IP，被标记为「强制重新配网」
         *
         * provisioning_run() 保存后会自己重启，正常情况下不返回。
         */
        if (validate_config()) {
            ESP_LOGW(TAG, "force provisioning requested (wifi may have changed)");
        }
        provisioning_run();
#else
        ESP_LOGE(TAG, "config invalid, halting here so the error stays visible");
#endif
        return;
    }

    const app_config_t *cfg = app_config_get();

    /* 按住 BOOT 上电 -> 本次不睡，串口全程可用，方便调试 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_APP_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    bool force_awake = (gpio_get_level(CONFIG_APP_BOOT_GPIO) == 0);
    if (force_awake) {
        ESP_LOGW(TAG, "BOOT held -> light sleep disabled for this boot");
    }

    ESP_LOGI(TAG, "config from  : %s", app_config_from_nvs() ? "nvs (web page)"
                                                             : "menuconfig");
    ESP_LOGI(TAG, "wifi ssid   : %s", cfg->wifi_ssid);
    ESP_LOGI(TAG, "bemfa topic : %s", cfg->bemfa_topic);
    ESP_LOGI(TAG, "target mac  : %02x:%02x:%02x:%02x:%02x:%02x",
             s_target_mac[0], s_target_mac[1], s_target_mac[2],
             s_target_mac[3], s_target_mac[4], s_target_mac[5]);
    ESP_LOGI(TAG, "magic dest  : %s:%d", cfg->broadcast, cfg->magic_port);
#ifdef CONFIG_APP_ENABLE_LIGHT_SLEEP
    ESP_LOGI(TAG, "light sleep : on");
#else
    ESP_LOGI(TAG, "light sleep : off");
#endif
#if CONFIG_APP_WIFI_PS_MAX_MODEM
    ESP_LOGI(TAG, "ps mode     : MAX_MODEM, listen interval = %d beacons",
             CONFIG_APP_WIFI_LISTEN_INTERVAL);
#else
    ESP_LOGI(TAG, "ps mode     : MIN_MODEM (wake on every DTIM)");
#endif

    ESP_ERROR_CHECK(power_init());

    ESP_ERROR_CHECK(wifi_mgr_start());

    ESP_LOGI(TAG, "waiting for ip ...");
    bool got_ip = wifi_mgr_wait_connected(30000);

#if CONFIG_APP_PROVISIONING
    /*
     * 连不上就多等两轮，总共 90 秒。还是不行，说明 WiFi 密码或 SSID 变了
     * 或者路由器换了，这时候自己回配网模式，用户拿手机重填就行，
     * 不用拆机箱接线刷固件。
     */
    for (int i = 0; i < 2 && !got_ip; i++) {
        ESP_LOGW(TAG, "no ip yet, retry %d/2 in background ...", i + 1);
        got_ip = wifi_mgr_wait_connected(30000);
    }

    if (!got_ip) {
        ESP_LOGE(TAG, "still no ip after 90s -> entering provisioning mode");
        ESP_LOGE(TAG, "connect the WOL-Setup hotspot and re-enter the WiFi password");
        app_config_set_force_prov(true);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
#else
    if (!got_ip) {
        ESP_LOGE(TAG, "no ip in 30s, will keep retrying in background");
    }
#endif

    if (bemfa_start(on_mqtt_cmd) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed");
        return;
    }

    if (!bemfa_wait_connected(15000)) {
        ESP_LOGW(TAG, "mqtt not connected yet, keep running anyway");
    }

    xTaskCreate(housekeeping_task, "housekeeping", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready. commands over mqtt: on / off / debug / sleep / status");

    /*
     * 一条启动摘要发到 MQTT。
     * light sleep 一旦开启，串口就彻底没声音了，这条摘要就是唯一的
     * 远程凭证：能从云端看到 IP，就说明 WiFi + MQTT 两条链路都活着。
     */
    {
        char ip[24] = "no-ip";
        char buf[96];
        wifi_mgr_get_ip(ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "ready:v" APP_VERSION ",ip=%s,ls=%d",
                 ip, FORCE_LIGHT_SLEEP_FLAG);
        bemfa_publish(buf);
    }

    if (force_awake) {
        ESP_LOGW(TAG, "stay awake (BOOT held)");
        return;
    }

#if CONFIG_APP_ENABLE_LIGHT_SLEEP
    ESP_LOGI(TAG, "grace period %d ms ...", CONFIG_APP_STARTUP_GRACE_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_STARTUP_GRACE_MS));

    if (power_enable_light_sleep() == ESP_OK) {
        wifi_mgr_apply_ps_mode();

        /*
         * 必须在 esp_wifi_start() 之后再设一次发射功率：
         * 部分 C3 SuperMini 在高功率下射频失真，表现为 AUTH_EXPIRE。
         */
        wifi_mgr_apply_tx_power();

        bemfa_publish("boot:sleeping");
    }
#endif

    /* app_main 返回后其余任务继续跑，这里不需要 while(1) */
}
