#pragma once
#include <stdbool.h>
#include "link_packet.h"
/* Owns NVS/netif/event loop/STA driver; returns without waiting for AP. */
esp_err_t wifi_link_init(char device_id, link_receive_cb_t cb);
bool wifi_link_is_connected(void);
esp_err_t wifi_link_send(const void *data, size_t len);
