/*
 * 配网模式
 *
 * 配置不完整时进入这里：开一个开放热点，跑一个 Web 服务，
 * 用户连上后在浏览器里填 WiFi / 巴法云 / 目标 MAC，保存后重启进正常模式。
 *
 * 三点设计取舍：
 *
 *   - 用 APSTA 而不是纯 AP：只有 STA 也起来了才能扫描周围 WiFi，
 *     否则页面上没法给用户一个可选列表，只能手打 SSID（中文很容易打错）。
 *
 *   - 自己实现 DNS 劫持：手机连上后会去探测是否联网，把任意域名都答成
 *     192.168.4.1，系统就会自动弹出配置页（captive portal）。
 *
 *   - 页面放独立文件 index.html，编译时嵌进固件。改文案不用碰 C 代码。
 */

#include "provisioning.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_config.h"

static const char *TAG = "prov";

#define AP_SSID_PREFIX  "WOL-Setup-"
#define AP_MAX_CONN     4
#define SCAN_MAX        20

/* 编译期嵌入的配置页 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static char s_ap_ssid[33];
static httpd_handle_t s_server = NULL;

/*
 * 扫描结果 20 * sizeof(wifi_ap_record_t) ≈ 2.2KB，必须静态放置。
 * 早期版本把它放在 sys_evt 任务栈上直接栈溢出 panic 过。
 */
static wifi_ap_record_t s_scan[SCAN_MAX];

/* ------------------------------------------------------------------ */
/* 表单解析                                                            */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 表单是 application/x-www-form-urlencoded：空格变 +，非 ASCII 走 %XX */
static bool url_decode(const char *src, size_t len, char *out, size_t cap)
{
    size_t o = 0;

    for (size_t i = 0; i < len; i++) {
        char c = src[i];

        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < len) {
            int hi = hexval(src[i + 1]);
            int lo = hexval(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }

        if (o + 1 >= cap) {
            return false;
        }
        out[o++] = c;
    }

    out[o] = '\0';
    return true;
}

static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p != NULL && *p != '\0') {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);

        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return url_decode(p + klen + 1, seg - klen - 1, out, cap);
        }

        p = amp ? amp + 1 : NULL;
    }

    if (cap > 0) {
        out[0] = '\0';
    }
    return false;
}

/* SSID 里可能出现引号或反斜杠，直接拼进 JSON 会破坏结构 */
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;

    for (const char *p = src; *p != '\0' && o + 3 < cap; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            dst[o++] = (char)c;
        }
    }

    dst[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTP 处理                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           (ssize_t)(index_html_end - index_html_start));
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "[]");
    }

    uint16_t num = SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&num, s_scan) != ESP_OK) {
        num = 0;
    }

    char out[1600];
    int p = snprintf(out, sizeof(out), "[");
    bool first = true;

    for (int i = 0; i < num && p < (int)sizeof(out) - 96; i++) {
        if (s_scan[i].ssid[0] == '\0') {
            continue;
        }

        char name[100];
        json_escape((const char *)s_scan[i].ssid, name, sizeof(name));

        p += snprintf(&out[p], sizeof(out) - (size_t)p, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                      first ? "" : ",", name, s_scan[i].rssi);
        first = false;
    }

    p += snprintf(&out[p], sizeof(out) - (size_t)p, "]");

    return httpd_resp_send(req, out, p);
}

static esp_err_t send_error_page(httpd_req_t *req, const char *msg)
{
    char page[640];

    snprintf(page, sizeof(page),
             "<!DOCTYPE html><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<body style=\"font-family:-apple-system,sans-serif;padding:26px;line-height:1.7\">"
             "<h3 style=\"color:#c0392b;margin:0 0 12px\">保存失败</h3>"
             "<p>%s</p><p><a href=\"/\">返回重新填写</a></p></body>", msg);

    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_LOGW(TAG, "restarting into normal mode");
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[768];
    int len = req->content_len;

    if (len <= 0 || len >= (int)sizeof(body)) {
        return send_error_page(req, "表单数据长度异常");
    }

    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return send_error_page(req, "读取表单失败，请重试");
        }
        got += r;
    }
    body[got] = '\0';

    app_config_t cfg = *app_config_get();

    if (!form_get(body, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid)) ||
        cfg.wifi_ssid[0] == '\0') {
        return send_error_page(req, "WiFi 名称不能为空");
    }

    /* 密码允许为空：家里有开放网络的情况虽然少见，但不能因此保存不了 */
    form_get(body, "pass", cfg.wifi_password, sizeof(cfg.wifi_password));

    if (!form_get(body, "uid", cfg.bemfa_uid, sizeof(cfg.bemfa_uid)) ||
        cfg.bemfa_uid[0] == '\0') {
        return send_error_page(req, "巴法云 UID 不能为空");
    }

    if (!form_get(body, "topic", cfg.bemfa_topic, sizeof(cfg.bemfa_topic)) ||
        cfg.bemfa_topic[0] == '\0') {
        return send_error_page(req, "巴法云主题名不能为空");
    }

    if (!form_get(body, "mac", cfg.target_mac, sizeof(cfg.target_mac)) ||
        !app_config_mac_valid(cfg.target_mac)) {
        return send_error_page(req, "MAC 格式不对，应为 AA:BB:CC:DD:EE:FF");
    }

    if (app_config_save(&cfg) != ESP_OK) {
        return send_error_page(req, "写入 flash 失败，请重试");
    }

    /* 新的 WiFi 参数已经写进去了，清掉「强制配网」标记，下次正常启动 */
    app_config_set_force_prov(false);

    ESP_LOGI(TAG, "saved: ssid=\"%s\" topic=\"%s\" mac=%s",
             cfg.wifi_ssid, cfg.bemfa_topic, cfg.target_mac);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<!DOCTYPE html><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<body style=\"font-family:-apple-system,sans-serif;padding:32px;line-height:1.8;text-align:center\">"
        "<h3 style=\"color:#1a7f37\">配置已保存</h3>"
        "<p>设备正在重启并连接 WiFi，热点即将关闭。</p>"
        "<p style=\"color:#888;font-size:14px\">大约 30 秒后，小爱同学就能叫醒电脑了。</p>"
        "</body>", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(restart_task, "cfg_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* captive portal：系统弹出探测请求时把浏览器导向配置页 */
static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* DNS 劫持                                                            */
/* ------------------------------------------------------------------ */

/*
 * 把所有 A 查询都答成 192.168.4.1。
 * 手机连上 AP 后会访问 connectivitycheck 之类的域名，
 * 拿到一个「有效」应答就会认为需要登录，自动弹出页面。
 */
static int build_dns_reply(const uint8_t *q, int qn, uint8_t *out, int cap)
{
    if (qn < 12 || cap < qn + 16) {
        return 0;
    }

    int p = 12;

    /* 跳过 QNAME 的标签序列 */
    while (p < qn && q[p] != 0) {
        p += q[p] + 1;
    }
    p++;      /* 结尾的 0 */
    p += 4;   /* QTYPE + QCLASS */

    if (p > qn) {
        return 0;
    }

    memcpy(out, q, (size_t)p);

    out[2] = 0x81;  /* QR=1, RD=1 */
    out[3] = 0x80;  /* RA=1 */
    out[4] = 0x00;
    out[5] = 0x01;  /* QDCOUNT = 1 */
    out[6] = 0x00;
    out[7] = 0x01;  /* ANCOUNT = 1 */
    out[8] = 0x00;
    out[9] = 0x00;
    out[10] = 0x00;
    out[11] = 0x00;

    int i = p;
    out[i++] = 0xC0; out[i++] = 0x0C;              /* 名字指向 offset 12 */
    out[i++] = 0x00; out[i++] = 0x01;              /* TYPE A */
    out[i++] = 0x00; out[i++] = 0x01;              /* CLASS IN */
    out[i++] = 0x00; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 60;                /* TTL 60s */
    out[i++] = 0x00; out[i++] = 0x04;              /* RDLENGTH */
    out[i++] = 192; out[i++] = 168; out[i++] = 4; out[i++] = 1;

    return i;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "dns hijack listening on udp/53");

    uint8_t in[512];
    uint8_t out[512];

    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);

        int n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            continue;
        }

        int rlen = build_dns_reply(in, n, out, sizeof(out));
        if (rlen > 0) {
            sendto(sock, out, (size_t)rlen, 0, (struct sockaddr *)&from, flen);
        }
    }
}

/* ------------------------------------------------------------------ */

static esp_err_t start_http(void)
{
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.lru_purge_enable = true;
    hc.max_open_sockets = 4;

    esp_err_t err = httpd_start(&s_server, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t fixed[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = root_get },
        { .uri = "/scan",   .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/save",   .method = HTTP_POST, .handler = save_post },
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        httpd_register_uri_handler(s_server, &fixed[i]);
    }

    /* 各家系统的联网探测路径，全部导向首页 */
    static const char *const probes[] = {
        "/generate_204", "/generate204", "/gen_204", "/mobile/status.php",
        "/hotspot-detect.html", "/library/test/success.html",
        "/success.txt", "/ncsi.txt", "/connecttest.txt", "/redirect",
        "/canonical.html", "/mtuprobe", "/favicon.ico",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        httpd_uri_t u = { .uri = probes[i], .method = HTTP_GET, .handler = redirect_get };
        httpd_register_uri_handler(s_server, &u);
    }

    return ESP_OK;
}

void provisioning_run(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    ESP_LOGW(TAG, "==============================================");
    ESP_LOGW(TAG, " config incomplete -> provisioning mode");
    ESP_LOGW(TAG, " hotspot : %s  (open, no password)", s_ap_ssid);
    ESP_LOGW(TAG, " open    : http://192.168.4.1");
    ESP_LOGW(TAG, "==============================================");

    /* 正常模式里 wifi_mgr 会初始化这些，配网路径要自己来 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, s_ap_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    wc.ap.channel = 6;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    wc.ap.beacon_interval = 100;

    /* APSTA：热点给用户连，STA 只用来扫描周边 WiFi 列表 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(start_http());

    xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "waiting for user to submit the form ...");

    /* 保存成功后会 esp_restart()，这里不需要退出条件 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
