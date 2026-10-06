#pragma once
#include <stdbool.h>
#include "link_packet.h"
esp_err_t espnow_link_init(const uint8_t peer_mac[6], link_receive_cb_t cb);
bool espnow_link_is_ready(void);
esp_err_t espnow_link_send(const void *data, size_t len);

/* MAC delivery diagnostics; application success still requires transport ACK. */
typedef struct {
    uint32_t mac_tx_ok;
    uint32_t mac_tx_fail;
    uint32_t raw_rx;
    uint32_t rejected_source;
} espnow_link_stats_t;
void espnow_link_get_stats(espnow_link_stats_t *out);
