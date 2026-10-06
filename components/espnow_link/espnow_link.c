#include "espnow_link.h"
#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
static uint8_t peer_address[6];
static link_receive_cb_t receive_cb;
static bool ready;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static espnow_link_stats_t stats;

static void on_send(const uint8_t *mac, esp_now_send_status_t result)
{
    if (!mac || memcmp(mac, peer_address, 6) != 0) return;
    portENTER_CRITICAL(&stats_lock);
    if (result == ESP_NOW_SEND_SUCCESS) ++stats.mac_tx_ok;
    else ++stats.mac_tx_fail;
    portEXIT_CRITICAL(&stats_lock);
}
void espnow_link_get_stats(espnow_link_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&stats_lock);
    *out = stats;
    portEXIT_CRITICAL(&stats_lock);
}

static void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    bool accepted = info && memcmp(info->src_addr, peer_address, 6) == 0;
    portENTER_CRITICAL(&stats_lock);
    ++stats.raw_rx;
    if (!accepted) ++stats.rejected_source;
    portEXIT_CRITICAL(&stats_lock);
    if (accepted && len > 0 && receive_cb) {
        /* Callback only queues a copy; no display, socket IO or logging here. */
        receive_cb(LINK_ESPNOW, data, (size_t)len);
    }
}

esp_err_t espnow_link_init(const uint8_t peer_mac[6], link_receive_cb_t cb)
{
    memcpy(peer_address, peer_mac, 6);
    receive_cb = cb;
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) return err;
    err = esp_now_register_recv_cb(on_receive);
    if (err != ESP_OK) { esp_now_deinit(); return err; }
    err = esp_now_register_send_cb(on_send);
    if (err != ESP_OK) { esp_now_deinit(); return err; }
    esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(peer.peer_addr, peer_address, 6);
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) { esp_now_deinit(); return err; }
    ready = true;
    ESP_LOGI("espnow_link", "Peer " MACSTR ", channel=current radio channel", MAC2STR(peer_address));
    return ESP_OK;
}
bool espnow_link_is_ready(void) { return ready; }
esp_err_t espnow_link_send(const void *data, size_t len)
{
    return ready ? esp_now_send(peer_address, data, len) : ESP_ERR_INVALID_STATE;
}
