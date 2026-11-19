#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 解析 "AA:BB:CC:DD:EE:FF" 或 "AA-BB-CC-DD-EE-FF" */
bool wol_parse_mac(const char *str, uint8_t mac[6]);

/* 全 0 / 全 FF 都视为无效，避免误配置后静默失败 */
bool wol_mac_is_valid(const uint8_t mac[6]);

/*
 * 发送 WOL 魔术包。
 * 载荷是 6 个 0xFF 加 16 次重复的 MAC，共 102 字节，走 UDP。
 * dest_ip 一般是 255.255.255.255，端口一般 9。
 */
esp_err_t wol_send(const uint8_t mac[6], const char *dest_ip, uint16_t port);
