#pragma once
/* All deployment settings live here. Password is never logged. */
#define LINK_WIFI_SSID "YOUR_WIFI_SSID"
#define LINK_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define LINK_USE_STATIC_IP 1
#define LINK_IP_A "192.168.0.100"
#define LINK_IP_B "192.168.0.101"
/* With DHCP, set these to DHCP reservations for the opposite board. */
#define LINK_DHCP_PEER_FOR_A "192.168.0.101"
#define LINK_DHCP_PEER_FOR_B "192.168.0.100"
#define LINK_NETMASK "255.255.255.0"
#define LINK_GATEWAY "192.168.0.1"
#define LINK_UDP_PORT 3333
/* Common rendezvous channel when disconnected; not forced while on AP. */
#define LINK_FALLBACK_CHANNEL 6
#define LINK_RECONNECT_MS 20000
#define LINK_CONNECT_WINDOW_MS 4000
#define LINK_INTERVAL_MS 1000
#define LINK_ACK_TIMEOUT_MS 800
#define LINK_FAIL_COUNT 3
/* Candidate promotion requires this many consecutive end-to-end ACKs (3-5). */
#define LINK_RECOVER_COUNT 4

/* Temporary detailed diagnostics; set to 0 after checking both boards. */
#define LINK_DIAGNOSTICS 1

/* Waveshare ESP32-S3-ETH onboard W5500, separate from TFT's SPI3. */
#define LINK_ETH_ENABLED 1
#define LINK_ETH_SPI_HOST SPI2_HOST
#define LINK_ETH_MOSI 11
#define LINK_ETH_MISO 12
#define LINK_ETH_SCLK 13
#define LINK_ETH_CS 14
#define LINK_ETH_INT 10
#define LINK_ETH_RST 9
#define LINK_ETH_SPI_MHZ 20
/* Same LAN as Wi-Fi, but each interface must have its own unique IP. */
#define LINK_ETH_USE_STATIC_IP 1
#define LINK_ETH_IP_A "192.168.0.110"
#define LINK_ETH_IP_B "192.168.0.111"
#define LINK_ETH_NETMASK "255.255.255.0"
#define LINK_ETH_GATEWAY "192.168.0.1"
#define LINK_ETH_DHCP_PEER_FOR_A "192.168.0.111"
#define LINK_ETH_DHCP_PEER_FOR_B "192.168.0.110"

/* Startup presentation only; DATA/PROBE/ACK continue immediately. */
#define LINK_STARTUP_MIN_MS 4000
#define LINK_STARTUP_STABLE_MS 3000
#define LINK_STARTUP_WAIT_NOTICE_MS 20000
