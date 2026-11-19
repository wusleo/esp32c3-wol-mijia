#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 巴法云 MQTT 客户端封装。
 *
 * 一个实测出来的硬性约束：client_id、username、password 三者都必须是 UID，
 * 其中任何一个换成别的值，服务器会直接回 CONNACK rc=5（not authorised）。
 * 也就是说没法在 PC 上另起一个 client 旁听同一个主题 —— 会被服务器拒绝。
 */

typedef void (*bemfa_cmd_cb_t)(const char *data, int len);

esp_err_t bemfa_start(bemfa_cmd_cb_t on_cmd);

/* 阻塞等待 MQTT 连接建立 */
bool bemfa_wait_connected(uint32_t timeout_ms);

bool bemfa_connected(void);

/* 失败时静默返回，不阻塞调用者 */
void bemfa_publish(const char *payload);
