#pragma once
#include <stdbool.h>
#include "link_packet.h"
typedef struct {
    char device_id;
    link_type_t active;
    bool peer_reachable;
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t queue_drops;
} transport_status_t;
esp_err_t transport_start(void);
/* Thread-safe snapshot; never calls display from a network callback. */
void transport_get_status(transport_status_t *out);
