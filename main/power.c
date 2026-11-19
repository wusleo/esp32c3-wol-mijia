#include "power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "power";

static esp_pm_lock_handle_t s_lock = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * 必须是 64 位的绝对时间，且必须被临界区保护。
 * C3 是 32 位核，volatile int64_t 的读写在编译产物里是两条 32 位指令，
 * 两个任务并发访问会读到撕裂值（高 32 位新、低 32 位旧），
 * 于是窗口可能提前关闭或者永远不关闭。
 */
static int64_t s_deadline_us = 0;
static bool s_sleep_on = false;

esp_err_t power_init(void)
{
    return esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "remote_debug", &s_lock);
}

bool power_light_sleep_on(void)
{
    return s_sleep_on;
}

esp_err_t power_enable_light_sleep(void)
{
#if CONFIG_APP_ENABLE_LIGHT_SLEEP
    /*
     * min_freq_mhz 必须等于 40：C3 的 XTAL 就是 40MHz，
     * 填别的值 esp_pm_configure 会返回 ESP_ERR_INVALID_ARG。
     */
    esp_pm_config_t cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };

    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sleep_on = true;

    /* 这行必须打印在静音之前：它是最后一次能看到的人话 */
    ESP_LOGW(TAG, "light sleep enabled; console goes silent from now on");
    ESP_LOGW(TAG, "publish \"debug\" over MQTT if you need to flash again");

    esp_log_level_set("*", ESP_LOG_NONE);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "CONFIG_APP_ENABLE_LIGHT_SLEEP=n, staying awake");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool power_awake_active(void)
{
    bool active;
    taskENTER_CRITICAL(&s_mux);
    active = (s_deadline_us != 0);
    taskEXIT_CRITICAL(&s_mux);
    return active;
}

esp_err_t power_awake_begin(uint32_t seconds)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool need_acquire = false;

    taskENTER_CRITICAL(&s_mux);
    if (s_deadline_us == 0) {
        need_acquire = true;
    }
    s_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    taskEXIT_CRITICAL(&s_mux);

    /* esp_pm_lock_acquire 内部可能加自己的锁，不能放在临界区里调用 */
    if (need_acquire) {
        return esp_pm_lock_acquire(s_lock);
    }
    return ESP_OK;
}

void power_awake_end(void)
{
    bool was_active = false;

    taskENTER_CRITICAL(&s_mux);
    if (s_deadline_us != 0) {
        s_deadline_us = 0;
        was_active = true;
    }
    taskEXIT_CRITICAL(&s_mux);

    if (was_active && s_lock != NULL) {
        esp_pm_lock_release(s_lock);
    }
}

void power_tick(void)
{
    bool expired = false;

    taskENTER_CRITICAL(&s_mux);
    if (s_deadline_us != 0 && esp_timer_get_time() > s_deadline_us) {
        s_deadline_us = 0;
        expired = true;
    }
    taskEXIT_CRITICAL(&s_mux);

    if (expired && s_lock != NULL) {
        esp_pm_lock_release(s_lock);
    }
}
