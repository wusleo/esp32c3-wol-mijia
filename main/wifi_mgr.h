#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * WiFi 连接管理。
 *
 * 设计要点：
 *   - 连接前先扫描，挑 SSID 相同、信号最强的那一个 BSSID 锁定。
 *     双频合一或者 Mesh 环境下，不锁 BSSID 很容易连到远端弱节点。
 *   - 断线重连放在专用任务里做，且每次重连都重新扫描。
 *     固件早期版本用「记住的 BSSID + 信道」直接重连，AP 一换信道就死循环。
 */

esp_err_t wifi_mgr_start(void);

/* 阻塞等待拿到 IP，超时返回 false */
bool wifi_mgr_wait_connected(uint32_t timeout_ms);

bool wifi_mgr_connected(void);

/* 必须在上电后尽早调用：某些 C3 SuperMini 在高功率下射频失真连不上 AP */
esp_err_t wifi_mgr_apply_tx_power(void);

/* 应用 menuconfig 里选择的 WiFi 省电模式 */
esp_err_t wifi_mgr_apply_ps_mode(void);

/* 取当前 IPv4 地址字符串，没连上返回 false */
bool wifi_mgr_get_ip(char *buf, size_t len);
