#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 电源管理封装。
 *
 * 两件事：
 *   1) 打开自动 light sleep（Arduino 的预编译库做不到，必须用 ESP-IDF）
 *   2) 提供一把「远程调试窗口」锁：拿到 ESP_PM_NO_LIGHT_SLEEP 后 CPU 不睡，
 *      但注意 —— USB PHY 已经掉线的情况下，光让 CPU 醒着并不会自动恢复串口，
 *      本模块只负责让芯片保持清醒，USB 是否可用取决于芯片是否重新枚举。
 */

esp_err_t power_init(void);

/* 打开 light sleep。成功后会把日志静音，见下方说明。 */
esp_err_t power_enable_light_sleep(void);

bool power_light_sleep_on(void);

/* ---- 远程调试窗口 ---- */

bool power_awake_active(void);
esp_err_t power_awake_begin(uint32_t seconds);
void power_awake_end(void);

/* 每秒调用一次，负责处理窗口到期后自动归还锁 */
void power_tick(void);

/*
 * 关于「进入睡眠后必须静音日志」：
 *
 * C3 的 console 走 USB-Serial/JTAG。sleep 之后主机侧已经认为设备拔出，
 * 此时任何往 stdout 的写入都会阻塞在驱动里（等一个永远不会来的主机），
 * 任务就此卡死。历史上就是因为 monitor 任务卡在写日志上，
 * 导致到期归还锁的代码永远执行不到，白白耗电。
 *
 * 所以 power_enable_light_sleep() 成功后会执行 esp_log_level_set("*", ESP_LOG_NONE)，
 * 之后整机不再输出任何日志。如果你想看日志，按住 BOOT 键上电即可跳过睡眠。
 */
