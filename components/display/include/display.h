#pragma once

#include <stdbool.h>
#include <stdint.h>

void display_init(void);

/* Normal task context only, just like display_update(). */
void display_connecting(char device_id, uint32_t elapsed_ms, bool waiting_peer);

void display_update(
    char device_id,
    const char *transport_name,
    bool peer_reachable,
    uint32_t tx_count,
    uint32_t rx_count);
