#include "wifi_link.h"
#include "link_config.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define ASSOCIATED BIT0
#define HAS_IP BIT1
#define SOCKET_REFRESH BIT2
static const char *TAG = "wifi_link";
static EventGroupHandle_t events;
static SemaphoreHandle_t socket_lock;
static int udp_socket = -1;
static esp_netif_t *sta_netif;
static struct sockaddr_in peer;
static link_receive_cb_t receive_cb;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(events, ASSOCIATED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, ASSOCIATED | HAS_IP);
        xEventGroupSetBits(events, SOCKET_REFRESH);
        ESP_LOGW(TAG, "AP disconnected; ESP-NOW remains enabled");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ip_event_got_ip_t *event = data;
        if (!event || event->esp_netif != sta_netif) return;
        xEventGroupClearBits(events, HAS_IP);
        xEventGroupSetBits(events, SOCKET_REFRESH);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        if (!event || event->esp_netif != sta_netif) return;
        xEventGroupSetBits(events, HAS_IP | SOCKET_REFRESH);
        uint8_t channel = 0; wifi_second_chan_t secondary;
        esp_err_t err = esp_wifi_get_channel(&channel, &secondary);
        if (err != ESP_OK) ESP_LOGW(TAG, "get channel: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "IP " IPSTR ", mask " IPSTR ", gateway " IPSTR ", AP channel %u",
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw), channel);
    }
}

static int open_socket(void)
{
    esp_netif_ip_info_t ip;
    struct ifreq iface = {0};
    if (esp_netif_get_ip_info(sta_netif, &ip) != ESP_OK || ip.ip.addr == 0 ||
        esp_netif_get_netif_impl_name(sta_netif, iface.ifr_name) != ESP_OK) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(LINK_UDP_PORT),
                                .sin_addr.s_addr = ip.ip.addr };
    /* Keep Wi-Fi probes on Wi-Fi even while Ethernet is available. */
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) < 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    ESP_LOGI(TAG, "UDP bound to %s, " IPSTR ":%d", iface.ifr_name, IP2STR(&ip.ip), LINK_UDP_PORT);
    return fd;
}

static void wifi_task(void *arg)
{
    int64_t next_attempt = 0, deadline = 0, next_socket = 0;
    bool attempting = false, fallback_set = false;
    int64_t next_reject_log = 0;
    for (;;) {
        int64_t now = now_ms();
        EventBits_t bits = xEventGroupGetBits(events);
        if (bits & HAS_IP) {
            attempting = false;
            fallback_set = false;
            next_attempt = now + LINK_RECONNECT_MS;
        } else if (attempting && now >= deadline) {
            /* Includes association with no DHCP lease. Stop scanning before setting channel. */
            esp_err_t err = esp_wifi_disconnect();
            if (err != ESP_OK) ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(err));
            attempting = false;
            next_attempt = now + LINK_RECONNECT_MS;
            fallback_set = false;
        } else if (!attempting && now >= next_attempt) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
            attempting = true;
            fallback_set = false;
            deadline = now + LINK_CONNECT_WINDOW_MS;
        }
        if (!(bits & ASSOCIATED) && !attempting && !fallback_set) {
            esp_err_t err = esp_wifi_set_channel(LINK_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
            if (err == ESP_OK) {
                fallback_set = true;
                ESP_LOGI(TAG, "Disconnected rendezvous channel %d", LINK_FALLBACK_CHANNEL);
            }
        }

        uint8_t buffer[sizeof(link_packet_t) + 1];
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int len = -1;
        bool refresh = (xEventGroupWaitBits(events, SOCKET_REFRESH, pdTRUE, pdFALSE, 0) & SOCKET_REFRESH) != 0;
        bool connected = wifi_link_is_connected();
        xSemaphoreTake(socket_lock, portMAX_DELAY);
        if ((refresh || !connected) && udp_socket >= 0) {
            close(udp_socket);
            udp_socket = -1;
            next_socket = 0;
        }
        if (connected && udp_socket < 0 && now >= next_socket) {
            udp_socket = open_socket();
            next_socket = now + 1000;
            if (udp_socket < 0) ESP_LOGW(TAG, "UDP socket unavailable: errno %d", errno);
        }
        if (udp_socket >= 0) {
            len = recvfrom(udp_socket, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&source, &source_len);
            if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGW(TAG, "UDP receive error: errno %d; recreating socket", errno);
                close(udp_socket);
                udp_socket = -1;
            }
        }
        xSemaphoreGive(socket_lock);
        if (len > 0) {
            if (source.sin_addr.s_addr == peer.sin_addr.s_addr && source.sin_port == peer.sin_port &&
                (xEventGroupGetBits(events) & HAS_IP)) {
                receive_cb(LINK_WIFI, buffer, (size_t)len);
            } else if (now >= next_reject_log) {
                next_reject_log = now + 5000;
                ESP_LOGW(TAG, "UDP rejected source=%s:%u, expected peer port=%d, has_ip=%d",
                         inet_ntoa(source.sin_addr), ntohs(source.sin_port), LINK_UDP_PORT,
                         (xEventGroupGetBits(events) & HAS_IP) != 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t wifi_link_init(char device_id, link_receive_cb_t cb)
{
    receive_cb = cb;
    events = xEventGroupCreate();
    socket_lock = xSemaphoreCreateMutex();
    if (!events || !socket_lock) return ESP_ERR_NO_MEM;
    /* Do not erase NVS silently on init failure. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) return ESP_ERR_NO_MEM;
    sta_netif = sta;
    if (LINK_USE_STATIC_IP) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_str_to_ip4(device_id == 'A' ? LINK_IP_A : LINK_IP_B, &ip.ip) != ESP_OK ||
            esp_netif_str_to_ip4(LINK_NETMASK, &ip.netmask) != ESP_OK ||
            esp_netif_str_to_ip4(LINK_GATEWAY, &ip.gw) != ESP_OK)
            return ESP_ERR_INVALID_ARG;
        err = esp_netif_dhcpc_stop(sta);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
        err = esp_netif_set_ip_info(sta, &ip);
        if (err != ESP_OK) return err;
    }
    const char *peer_ip = LINK_USE_STATIC_IP ? (device_id == 'A' ? LINK_IP_B : LINK_IP_A) :
        (device_id == 'A' ? LINK_DHCP_PEER_FOR_A : LINK_DHCP_PEER_FOR_B);
    peer = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(LINK_UDP_PORT) };
    if (inet_pton(AF_INET, peer_ip, &peer.sin_addr) != 1) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "Board %c, %s addressing, UDP peer=%s:%d", device_id,
             LINK_USE_STATIC_IP ? "static" : "DHCP", peer_ip, LINK_UDP_PORT);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) return err;
    wifi_config_t config = {0};
    if (strlen(LINK_WIFI_SSID) > sizeof(config.sta.ssid) ||
        strlen(LINK_WIFI_PASSWORD) > sizeof(config.sta.password)) return ESP_ERR_INVALID_ARG;
    memcpy(config.sta.ssid, LINK_WIFI_SSID, strlen(LINK_WIFI_SSID));
    memcpy(config.sta.password, LINK_WIFI_PASSWORD, strlen(LINK_WIFI_PASSWORD));
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) return err;
    return xTaskCreate(wifi_task, "wifi_link", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool wifi_link_is_connected(void) { return (xEventGroupGetBits(events) & HAS_IP) != 0; }
esp_err_t wifi_link_send(const void *data, size_t len)
{
    if (!wifi_link_is_connected()) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(socket_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int fd = udp_socket;
    int sent = fd < 0 ? -1 : sendto(fd, data, len, 0, (struct sockaddr *)&peer, sizeof(peer));
    int send_errno = sent < 0 && fd >= 0 ? errno : 0;
    xSemaphoreGive(socket_lock);
    if (sent != (int)len) {
        ESP_LOGW(TAG, "UDP send to %s:%d failed: socket=%d, errno=%d (%s)",
                 inet_ntoa(peer.sin_addr), LINK_UDP_PORT, fd, send_errno,
                 fd < 0 ? "socket unavailable" : strerror(send_errno));
        return fd < 0 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    return ESP_OK;
}
