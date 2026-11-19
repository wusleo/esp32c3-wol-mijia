#include "wol.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "wol";

#define MAGIC_REPEAT 16
#define MAGIC_PKT_LEN (6 + MAGIC_REPEAT * 6)

bool wol_parse_mac(const char *str, uint8_t mac[6])
{
    unsigned v[6];

    if (str == NULL || mac == NULL) {
        return false;
    }

    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
        sscanf(str, "%x-%x-%x-%x-%x-%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) {
            return false;
        }
        mac[i] = (uint8_t)v[i];
    }

    return true;
}

bool wol_mac_is_valid(const uint8_t mac[6])
{
    if (mac == NULL) {
        return false;
    }

    bool all_zero = true;
    bool all_ff = true;

    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xFF) all_ff = false;
    }

    return !all_zero && !all_ff;
}

/*
 * 从当前 WiFi 的 IP + 掩码推出子网广播地址（192.168.1.43/24 -> 192.168.1.255）。
 * 配网页里广播地址留空时就靠它，用户不用自己算。
 */
static bool auto_broadcast(char out[16])
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK) {
        return false;
    }

    if (ip.ip.addr == 0 || ip.netmask.addr == 0) {
        return false;
    }

    esp_ip4_addr_t b;
    b.addr = (ip.ip.addr & ip.netmask.addr) | ~ip.netmask.addr;
    snprintf(out, 16, IPSTR, IP2STR(&b));
    return true;
}

esp_err_t wol_send(const uint8_t mac[6], const char *dest_ip, uint16_t port)
{
    if (!wol_mac_is_valid(mac)) {
        ESP_LOGE(TAG, "refuse to send: invalid target MAC");
        return ESP_ERR_INVALID_ARG;
    }

    char auto_dst[16];
    const char *dst = dest_ip;

    if (dst == NULL || dst[0] == '\0' || strcmp(dst, "auto") == 0) {
        if (!auto_broadcast(auto_dst)) {
            ESP_LOGE(TAG, "no broadcast address and none could be derived");
            return ESP_ERR_INVALID_STATE;
        }
        dst = auto_dst;
        ESP_LOGI(TAG, "broadcast address derived from netif: %s", dst);
    }

    /*
     * 必须用 inet_aton，不能用 inet_addr：
     * inet_addr("255.255.255.255") 返回 0xFFFFFFFF，正好等于 INADDR_NONE，
     * 于是受限广播地址会被当成非法地址拒掉 —— 这是 inet_addr 的历史缺陷。
     */
    struct in_addr addr;
    if (inet_aton(dst, &addr) == 0) {
        ESP_LOGE(TAG, "bad destination: %s", dst);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pkt[MAGIC_PKT_LEN];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < MAGIC_REPEAT; i++) {
        memcpy(&pkt[6 + i * 6], mac, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return ESP_FAIL;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr = addr;

    int sent = sendto(sock, pkt, sizeof(pkt), 0,
                      (struct sockaddr *)&to, sizeof(to));

    close(sock);

    if (sent != (int)sizeof(pkt)) {
        ESP_LOGE(TAG, "sendto returned %d", sent);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "magic packet sent to %s:%u for "
                  "%02x:%02x:%02x:%02x:%02x:%02x",
             dst, port,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}
