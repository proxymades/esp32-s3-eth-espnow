#include "ethernet_link.h"
#include "link_config.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define LINK_UP BIT0
#define HAS_IP BIT1
#define SOCKET_REFRESH BIT2
static const char *TAG = "ethernet_link";
static EventGroupHandle_t events;
static SemaphoreHandle_t socket_lock;
static esp_netif_t *netif;
static esp_eth_handle_t driver;
static esp_eth_netif_glue_handle_t glue;
static esp_eth_mac_t *mac;
static esp_eth_phy_t *phy;
static int udp_socket = -1;
static struct sockaddr_in peer;
static link_receive_cb_t receive_cb;
static bool initialized;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void cleanup_result(esp_err_t err, const char *operation)
{
    if (err != ESP_OK) ESP_LOGW(TAG, "Cleanup %s: %s", operation, esp_err_to_name(err));
}

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT) {
        if (!data || *(esp_eth_handle_t *)data != driver) return;
        if (id == ETHERNET_EVENT_CONNECTED) {
            xEventGroupSetBits(events, LINK_UP);
            ESP_LOGI(TAG, "Cable link UP; awaiting usable IP and peer UDP ACK");
        } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
            xEventGroupClearBits(events, LINK_UP | HAS_IP);
            xEventGroupSetBits(events, SOCKET_REFRESH);
            ESP_LOGW(TAG, "Cable link DOWN; wireless transports stay available");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = data;
        if (!event || event->esp_netif != netif) return;
        xEventGroupSetBits(events, HAS_IP | SOCKET_REFRESH);
        ESP_LOGI(TAG, "IP " IPSTR ", mask " IPSTR ", gateway " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        ip_event_got_ip_t *event = data;
        if (!event || event->esp_netif != netif) return;
        xEventGroupClearBits(events, HAS_IP);
        xEventGroupSetBits(events, SOCKET_REFRESH);
        ESP_LOGW(TAG, "Ethernet IP lost");
    }
}

static int open_socket(void)
{
    esp_netif_ip_info_t ip;
    struct ifreq iface = {0};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0 ||
        esp_netif_get_netif_impl_name(netif, iface.ifr_name) != ESP_OK) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    /* Bind BOTH interface and source IP: the default route may be Wi-Fi. */
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(LINK_UDP_PORT),
                                .sin_addr.s_addr = ip.ip.addr };
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) < 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    ESP_LOGI(TAG, "UDP bound to %s, " IPSTR ":%d", iface.ifr_name, IP2STR(&ip.ip), LINK_UDP_PORT);
    return fd;
}

static void ethernet_task(void *arg)
{
    int64_t next_socket = 0, next_reject_log = 0;
    for (;;) {
        int64_t now = now_ms();
        bool refresh = (xEventGroupWaitBits(events, SOCKET_REFRESH, pdTRUE, pdFALSE, 0) & SOCKET_REFRESH) != 0;
        bool connected = ethernet_link_is_connected();
        uint8_t buffer[sizeof(link_packet_t) + 1];
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int len = -1;
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
        if (len > 0 && ethernet_link_is_connected()) {
            if (source.sin_addr.s_addr == peer.sin_addr.s_addr && source.sin_port == peer.sin_port) {
                receive_cb(LINK_ETHERNET, buffer, (size_t)len);
            } else if (now >= next_reject_log) {
                next_reject_log = now + 5000;
                ESP_LOGW(TAG, "UDP rejected source=%s:%u", inet_ntoa(source.sin_addr), ntohs(source.sin_port));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t ethernet_link_init(char device_id, link_receive_cb_t cb)
{
    if (!LINK_ETH_ENABLED) return ESP_ERR_NOT_SUPPORTED;
    if (initialized || events) return ESP_ERR_INVALID_STATE;
    if ((device_id != 'A' && device_id != 'B') || !cb) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    bool bus_owned = false, isr_owned = false, started = false;
    esp_event_handler_instance_t eth_handler = NULL, got_ip_handler = NULL, lost_ip_handler = NULL;
    receive_cb = cb;
    events = xEventGroupCreate();
    socket_lock = xSemaphoreCreateMutex();
    if (!events || !socket_lock) { err = ESP_ERR_NO_MEM; goto fail; }
    err = gpio_install_isr_service(0);
    if (err == ESP_OK) isr_owned = true;
    else if (err != ESP_ERR_INVALID_STATE) goto fail;
    spi_bus_config_t bus = { .mosi_io_num = LINK_ETH_MOSI, .miso_io_num = LINK_ETH_MISO,
        .sclk_io_num = LINK_ETH_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1 };
    err = spi_bus_initialize(LINK_ETH_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) goto fail;
    bus_owned = true;
    spi_device_interface_config_t spi = { .mode = 0, .clock_speed_hz = LINK_ETH_SPI_MHZ * 1000000,
        .spics_io_num = LINK_ETH_CS, .queue_size = 20 };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(LINK_ETH_SPI_HOST, &spi);
    w5500.int_gpio_num = LINK_ETH_INT;
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;
    phy_config.reset_gpio_num = LINK_ETH_RST;
    mac = esp_eth_mac_new_w5500(&w5500, &mac_config);
    phy = esp_eth_phy_new_w5500(&phy_config);
    if (!mac || !phy) { err = ESP_ERR_NO_MEM; goto fail; }
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&config, &driver);
    if (err != ESP_OK) goto fail;
    uint8_t eth_mac[6];
    err = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (err != ESP_OK) goto fail;
    err = esp_eth_ioctl(driver, ETH_CMD_S_MAC_ADDR, eth_mac);
    if (err != ESP_OK) goto fail;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    netif = esp_netif_new(&netif_config);
    if (!netif) { err = ESP_ERR_NO_MEM; goto fail; }
    if (LINK_ETH_USE_STATIC_IP) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_str_to_ip4(device_id == 'A' ? LINK_ETH_IP_A : LINK_ETH_IP_B, &ip.ip) != ESP_OK ||
            esp_netif_str_to_ip4(LINK_ETH_NETMASK, &ip.netmask) != ESP_OK ||
            esp_netif_str_to_ip4(LINK_ETH_GATEWAY, &ip.gw) != ESP_OK) {
            err = ESP_ERR_INVALID_ARG; goto fail;
        }
        err = esp_netif_dhcpc_stop(netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) goto fail;
        err = esp_netif_set_ip_info(netif, &ip);
        if (err != ESP_OK) goto fail;
    }
    const char *peer_ip = LINK_ETH_USE_STATIC_IP ? (device_id == 'A' ? LINK_ETH_IP_B : LINK_ETH_IP_A) :
        (device_id == 'A' ? LINK_ETH_DHCP_PEER_FOR_A : LINK_ETH_DHCP_PEER_FOR_B);
    peer = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(LINK_UDP_PORT) };
    if (inet_pton(AF_INET, peer_ip, &peer.sin_addr) != 1) { err = ESP_ERR_INVALID_ARG; goto fail; }
    glue = esp_eth_new_netif_glue(driver);
    if (!glue) { err = ESP_ERR_NO_MEM; goto fail; }
    err = esp_netif_attach(netif, glue);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL, &eth_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event, NULL, &got_ip_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, eth_event, NULL, &lost_ip_handler);
    if (err != ESP_OK) goto fail;
    err = esp_eth_start(driver);
    if (err != ESP_OK) goto fail;
    started = true;
    if (xTaskCreate(ethernet_task, "ethernet_link", 4096, NULL, 4, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM; goto fail;
    }
    /* Initialization is one-shot; cable/IP readiness is tracked in event bits. */
    initialized = true;
    ESP_LOGI(TAG, "W5500 SPI2, MAC " MACSTR ", board %c, %s IP, UDP peer=%s:%d",
             MAC2STR(eth_mac), device_id, LINK_ETH_USE_STATIC_IP ? "static" : "DHCP", peer_ip, LINK_UDP_PORT);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Initialization failed: %s; keeping wireless transports", esp_err_to_name(err));
    if (started) cleanup_result(esp_eth_stop(driver), "driver stop");
    if (eth_handler) cleanup_result(esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_handler), "ETH handler");
    if (got_ip_handler) cleanup_result(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler), "got-IP handler");
    if (lost_ip_handler) cleanup_result(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, lost_ip_handler), "lost-IP handler");
    if (glue) { cleanup_result(esp_eth_del_netif_glue(glue), "netif glue"); glue = NULL; }
    if (netif) { esp_netif_destroy(netif); netif = NULL; }
    if (driver) { cleanup_result(esp_eth_driver_uninstall(driver), "driver uninstall"); driver = NULL; }
    if (mac) { cleanup_result(mac->del(mac), "MAC delete"); mac = NULL; }
    if (phy) { cleanup_result(phy->del(phy), "PHY delete"); phy = NULL; }
    if (bus_owned) cleanup_result(spi_bus_free(LINK_ETH_SPI_HOST), "SPI bus");
    if (isr_owned) gpio_uninstall_isr_service();
    if (events) { vEventGroupDelete(events); events = NULL; }
    if (socket_lock) { vSemaphoreDelete(socket_lock); socket_lock = NULL; }
    return err;
}

bool ethernet_link_is_connected(void)
{
    if (!events) return false;
    return (xEventGroupGetBits(events) & (LINK_UP | HAS_IP)) == (LINK_UP | HAS_IP);
}
esp_err_t ethernet_link_send(const void *data, size_t len)
{
    if (!ethernet_link_is_connected()) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(socket_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int fd = udp_socket;
    int sent = fd < 0 ? -1 : sendto(fd, data, len, 0, (struct sockaddr *)&peer, sizeof(peer));
    int send_errno = sent < 0 && fd >= 0 ? errno : 0;
    xSemaphoreGive(socket_lock);
    if (sent != (int)len) {
        ESP_LOGW(TAG, "UDP send to %s:%d failed: socket=%d errno=%d (%s)",
                 inet_ntoa(peer.sin_addr), LINK_UDP_PORT, fd, send_errno,
                 fd < 0 ? "socket unavailable" : strerror(send_errno));
        return fd < 0 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    return ESP_OK;
}
