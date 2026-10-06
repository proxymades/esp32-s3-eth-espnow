#pragma once
#include <stdbool.h>
#include "link_packet.h"
/* Called after wifi_link_init establishes the shared netif/event infrastructure. */
esp_err_t ethernet_link_init(char device_id, link_receive_cb_t cb);
bool ethernet_link_is_connected(void);
esp_err_t ethernet_link_send(const void *data, size_t len);
