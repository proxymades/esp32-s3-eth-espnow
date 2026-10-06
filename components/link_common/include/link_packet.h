#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Fixed wire layout; both boards use the same little-endian firmware. */
typedef enum { LINK_ESPNOW = 0, LINK_WIFI, LINK_ETHERNET, LINK_COUNT } link_type_t;
typedef enum { PACKET_DATA = 1, PACKET_PROBE, PACKET_ACK } packet_kind_t;
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t session;
    uint32_t seq;
    uint32_t ack_session;
    uint8_t version;
    uint8_t sender;
    uint8_t transport;
    uint8_t kind;
} link_packet_t;
#define LINK_MAGIC UINT32_C(0x41424C4B)
#define LINK_VERSION 1
_Static_assert(sizeof(link_packet_t) == 20, "wire layout changed");
typedef void (*link_receive_cb_t)(link_type_t link, const void *data, size_t len);
