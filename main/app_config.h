/*
 * 运行时配置
 *
 * 配置有三个来源，优先级从高到低：
 *
 *   1. NVS（配网页面写入的）        <- 最终用户自己填的
 *   2. menuconfig（Kconfig）        <- 烧录者的出厂默认
 *   3. 空                           <- 两者的必填项都不全
 *
 * 前两个合并后得到一份运行时配置，各模块只认这一份，
 * 不再直接引用 CONFIG_APP_* 宏。这样同一份固件既能「烧完配网」，
 * 也能「编译期写死」，不影响已有的刷机习惯。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define APP_CFG_SSID_MAX    33   /* 32 字节 + NUL */
#define APP_CFG_PASS_MAX    65   /* 64 字节 + NUL */
#define APP_CFG_UID_MAX     40
#define APP_CFG_TOPIC_MAX   64
#define APP_CFG_MAC_MAX     18   /* "AA:BB:CC:DD:EE:FF" + NUL */
#define APP_CFG_BCAST_MAX   16

typedef struct {
    char wifi_ssid[APP_CFG_SSID_MAX];
    char wifi_password[APP_CFG_PASS_MAX];
    char bemfa_uid[APP_CFG_UID_MAX];
    char bemfa_topic[APP_CFG_TOPIC_MAX];
    char target_mac[APP_CFG_MAC_MAX];
    char broadcast[APP_CFG_BCAST_MAX];
    int  magic_port;
} app_config_t;

/*
 * 读 NVS 并与 Kconfig 默认值合并。必须在 nvs_flash_init() 之后调用。
 * 永远返回 ESP_OK：读不到就退化成 Kconfig，不会让设备起不来。
 */
esp_err_t app_config_load(void);

const app_config_t *app_config_get(void);

/* 必填项是否齐全：SSID / UID / topic / 可用 MAC 都就绪才算 */
bool app_config_complete(void);

/* 这份配置是从 NVS 读出来的吗（而不是 menuconfig 默认值） */
bool app_config_from_nvs(void);

/* 整体写入 NVS 并立即生效，失败时运行时配置不变 */
esp_err_t app_config_save(const app_config_t *cfg);

/* 清空 NVS 配置，下次启动会退回 menuconfig 或配网页面 */
esp_err_t app_config_erase(void);

/* AA:BB:CC:DD:EE:FF 或 AA-BB-... 格式，且不能是全 0 / 全 FF */
bool app_config_mac_valid(const char *mac);

/*
 * 「下次启动强制进配网」标志。
 *
 * 用途：家里换了 WiFi 密码，设备一直连不上，又没法拆机箱刷固件。
 * 启动时连续几次拿不到 IP 就置这个标志并重启，下次开机直接开热点，
 * 用户在手机上重填密码即可，配置不会丢（其他字段还在 NVS 里）。
 *
 * 配网页保存成功后必须清掉，否则会一直进配网。
 */
bool app_config_force_prov(void);
esp_err_t app_config_set_force_prov(bool on);
