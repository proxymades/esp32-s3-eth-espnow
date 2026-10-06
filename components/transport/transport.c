#include "transport.h"
#include "link_config.h"
#include "wifi_link.h"
#include "ethernet_link.h"
#include "espnow_link.h"
#include "display.h"
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "transport";
static const uint8_t MAC_A[6] = {0x3C, 0x0F, 0x02, 0xD7, 0x86, 0x74};
static const uint8_t MAC_B[6] = {0x3C, 0x0F, 0x02, 0xD8, 0x15, 0x70};

typedef struct {
    link_type_t type;
    const char *name;
    bool (*ready)(void);
    esp_err_t (*send)(const void *, size_t);
} link_adapter_t;
/* Ordered by priority; promotion always requires application ACKs. */
static const link_adapter_t adapters[] = {
    { LINK_ETHERNET, "ETHERNET", ethernet_link_is_connected, ethernet_link_send },
    { LINK_WIFI, "WI-FI", wifi_link_is_connected, wifi_link_send },
    { LINK_ESPNOW, "ESP-NOW", espnow_link_is_ready, espnow_link_send },
};
#define ADAPTER_COUNT (sizeof(adapters) / sizeof(adapters[0]))
_Static_assert(LINK_RECOVER_COUNT >= 3 && LINK_RECOVER_COUNT <= 5,
               "Promotion must require 3-5 consecutive end-to-end ACKs");
typedef struct {
    bool pending, healthy;
    uint8_t kind;
    unsigned failures, successes;
    uint32_t seq;
    int64_t deadline, last_ack;
} link_health_t;
typedef struct { link_type_t link; size_t received_len; link_packet_t packet; } receive_event_t;
static QueueHandle_t receive_queue;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static transport_status_t published;
static uint32_t queue_drops;
/* These variables are owned exclusively by the transport task. */
static transport_status_t status;
static link_health_t health[LINK_COUNT];
static uint32_t session, sequence, peer_session, peer_sequence;
static bool peer_seen;
static char peer_id;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static const link_adapter_t *adapter(link_type_t type)
{
    for (size_t i = 0; i < ADAPTER_COUNT; ++i) if (adapters[i].type == type) return &adapters[i];
    return NULL;
}
static void on_receive(link_type_t link, const void *data, size_t len)
{
    receive_event_t event = { .link = link, .received_len = len };
    memcpy(&event.packet, data, len < sizeof(event.packet) ? len : sizeof(event.packet));
    if (xQueueSend(receive_queue, &event, 0) != pdTRUE) {
        portENTER_CRITICAL(&status_lock);
        ++queue_drops;
        portEXIT_CRITICAL(&status_lock);
    }
}
static void publish(void)
{
    portENTER_CRITICAL(&status_lock);
    published = status;
    published.queue_drops = queue_drops;
    portEXIT_CRITICAL(&status_lock);
}
void transport_get_status(transport_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&status_lock);
    *out = published;
    portEXIT_CRITICAL(&status_lock);
}
static void failed(link_health_t *h)
{
    h->pending = false;
    h->successes = 0;
    if (h->failures < LINK_FAIL_COUNT) ++h->failures;
    if (h->failures >= LINK_FAIL_COUNT) h->healthy = false;
}
static void process_receive(const receive_event_t *event)
{
    const link_packet_t *p = &event->packet;
    const link_adapter_t *a = adapter(event->link);
    static int64_t next_reject_log[LINK_COUNT];
    if (!a) return;
    if (event->received_len != sizeof(*p) || p->magic != LINK_MAGIC || p->version != LINK_VERSION ||
        p->sender != peer_id || p->transport != event->link ||
        (p->kind != PACKET_DATA && p->kind != PACKET_PROBE && p->kind != PACKET_ACK)) {
        if (now_ms() >= next_reject_log[event->link]) {
            next_reject_log[event->link] = now_ms() + 5000;
            ESP_LOGW(TAG, "%s rejected packet: len=%u magic=0x%08" PRIx32
                     " version=%u sender=%u transport=%u kind=%u; check peer firmware",
                     a->name, (unsigned)event->received_len, p->magic, p->version,
                     p->sender, p->transport, p->kind);
        }
        return;
    }
    link_health_t *h = &health[event->link];
    if (p->kind == PACKET_ACK) {
        /* A valid, timely ACK proves delivery to the other application's task. */
        if (h->pending && p->ack_session == session && p->seq == h->seq &&
            now_ms() < h->deadline) {
            h->pending = false;
            h->last_ack = now_ms();
            h->failures = 0;
            if (h->successes < LINK_RECOVER_COUNT) ++h->successes;
            if (h->successes >= LINK_RECOVER_COUNT) h->healthy = true;
            if (h->kind == PACKET_DATA) ++status.tx_count;
            if (LINK_DIAGNOSTICS) ESP_LOGI(TAG, "%s ACK seq=%" PRIu32 " accepted, streak=%u/%u",
                                          a->name, p->seq, h->successes, LINK_RECOVER_COUNT);
        } else {
            ESP_LOGW(TAG, "%s ACK rejected: seq=%" PRIu32 " expected=%" PRIu32
                     " session=%" PRIu32 " expected=%" PRIu32 " pending=%d late=%d",
                     a->name, p->seq, h->seq, p->ack_session, session, h->pending,
                     now_ms() >= h->deadline);
        }
        return;
    }
    if (p->kind != PACKET_DATA && p->kind != PACKET_PROBE) return;
    if (p->ack_session != 0) return;
    link_packet_t ack = { .magic = LINK_MAGIC, .version = LINK_VERSION,
        .session = session, .seq = p->seq, .ack_session = p->session,
        .sender = status.device_id, .transport = event->link, .kind = PACKET_ACK };
    esp_err_t err = a->send(&ack, sizeof(ack));
    if (err != ESP_OK) ESP_LOGW(TAG, "%s ACK send: %s", a->name, esp_err_to_name(err));
    if (LINK_DIAGNOSTICS) ESP_LOGI(TAG, "%s RX %s seq=%" PRIu32 " from=%c", a->name,
             p->kind == PACKET_DATA ? "DATA" : "PROBE", p->seq, p->sender);
    /* ACK duplicates too, but count each data sequence only once across transports. */
    if (p->kind == PACKET_DATA && (!peer_seen || p->session != peer_session ||
        (int32_t)(p->seq - peer_sequence) > 0)) {
        peer_seen = true;
        peer_session = p->session;
        peer_sequence = p->seq;
        ++status.rx_count;
    }
}
static void update_health(int64_t now)
{
    for (size_t i = 0; i < ADAPTER_COUNT; ++i) {
        const link_adapter_t *a = &adapters[i];
        link_health_t *h = &health[a->type];
        if (!a->ready()) {
            h->healthy = false;
            h->successes = 0;
            if (h->pending) failed(h);
        } else if (h->pending && now >= h->deadline) {
            failed(h);
            ESP_LOGW(TAG, "%s ACK timeout (%u/%u)", a->name, h->failures, LINK_FAIL_COUNT);
        }
        /* Only backups expire by age. Active failure is determined by its own
         * DATA ACK timeouts/send errors or loss of its physical/IP readiness. */
        if (a->type != status.active && (h->healthy || h->successes) &&
            now - h->last_ack > LINK_FAIL_COUNT * LINK_INTERVAL_MS + LINK_ACK_TIMEOUT_MS) {
            h->healthy = false;
            h->successes = 0;
        }
    }
}
static const link_adapter_t *select_active(void)
{
    size_t current_index = ADAPTER_COUNT;
    for (size_t i = 0; i < ADAPTER_COUNT; ++i) {
        if (adapters[i].type == status.active) {
            current_index = i;
            break;
        }
    }

    /* A failed candidate only resets its own streak. Promotion requires a
     * fresh full streak, even if this backup was healthy earlier. */
    for (size_t i = 0; i < current_index; ++i) {
        const link_adapter_t *candidate = &adapters[i];
        const link_health_t *h = &health[candidate->type];
        if (candidate->ready() && h->healthy && h->failures == 0 &&
            h->successes >= LINK_RECOVER_COUNT) return candidate;
    }

    if (current_index < ADAPTER_COUNT) {
        const link_adapter_t *current = &adapters[current_index];
        if (current->ready() && health[current->type].healthy) return current;

        /* Downgrade only after the current link itself has failed. An already
         * verified standby retains its own normal failure tolerance. */
        for (size_t i = current_index + 1; i < ADAPTER_COUNT; ++i) {
            const link_adapter_t *backup = &adapters[i];
            if (backup->ready() && health[backup->type].healthy) return backup;
        }
    }
    /* No verified route: keep trying the lowest-priority fallback. */
    return &adapters[ADAPTER_COUNT - 1];
}
static void send_packet(const link_adapter_t *a, uint8_t kind, int64_t now)
{
    link_health_t *h = &health[a->type];
    if (!a->ready() || h->pending) return;
    link_packet_t packet = { .magic = LINK_MAGIC, .version = LINK_VERSION,
        .session = session, .seq = ++sequence, .sender = status.device_id,
        .transport = a->type, .kind = kind };
    h->pending = true;
    h->kind = kind;
    h->seq = packet.seq;
    h->deadline = now + LINK_ACK_TIMEOUT_MS;
    esp_err_t err = a->send(&packet, sizeof(packet));
    if (err != ESP_OK) {
        failed(h);
        ESP_LOGW(TAG, "%s send: %s", a->name, esp_err_to_name(err));
    }
}
static void transport_task(void *arg)
{
    int64_t next_send = 0, next_display = 0, next_diagnostics = 0;
    int64_t startup_begin = now_ms(), stable_since = -1;
    link_type_t startup_candidate = LINK_COUNT;
    bool startup_complete = false;
    for (;;) {
        receive_event_t event;
        if (xQueueReceive(receive_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            process_receive(&event);
            /* Bound drain time so a busy receive queue cannot starve failover. */
            for (unsigned i = 0; i < 16 && xQueueReceive(receive_queue, &event, 0) == pdTRUE; ++i)
                process_receive(&event);
        }
        int64_t now = now_ms();
        update_health(now);
        const link_adapter_t *active = select_active();
        if (status.active != active->type) {
            status.active = active->type;
            ESP_LOGI(TAG, "Active transport -> %s", active->name);
            next_display = 0;
        }
        status.peer_reachable = active->ready() && health[active->type].healthy;
        /* Gate only the first status screen, never network exchange or failover. */
        if (!startup_complete) {
            if (status.peer_reachable && health[active->type].failures == 0) {
                if (startup_candidate != active->type || stable_since < 0) {
                    startup_candidate = active->type;
                    stable_since = now;
                }
                if (now - startup_begin >= LINK_STARTUP_MIN_MS &&
                    now - stable_since >= LINK_STARTUP_STABLE_MS) {
                    startup_complete = true;
                    next_display = 0;
                    ESP_LOGI(TAG, "Startup settled -> %s", active->name);
                }
            } else {
                stable_since = -1;
                startup_candidate = LINK_COUNT;
            }
        }
        if (now >= next_send) {
            next_send = now + LINK_INTERVAL_MS;
            send_packet(active, PACKET_DATA, now);
            /* Keep both IP links verified, including Wi-Fi standby under Ethernet. */
            for (size_t i = 0; i < ADAPTER_COUNT; ++i) {
                if (&adapters[i] != active && adapters[i].type != LINK_ESPNOW)
                    send_packet(&adapters[i], PACKET_PROBE, now);
            }
        }
        publish();
        if (LINK_DIAGNOSTICS && now >= next_diagnostics) {
            next_diagnostics = now + 5000;
            espnow_link_stats_t stats;
            espnow_link_get_stats(&stats);
            uint8_t channel = 0; wifi_second_chan_t secondary;
            esp_err_t err = esp_wifi_get_channel(&channel, &secondary);
            if (err != ESP_OK) ESP_LOGW(TAG, "Radio channel unavailable: %s", esp_err_to_name(err));
            transport_status_t snapshot;
            transport_get_status(&snapshot);
            ESP_LOGI(TAG, "DIAG board=%c channel=%u wifi_ip=%d eth_ip=%d active=%s TX=%" PRIu32
                     " RX=%" PRIu32 " esp_mac_ok=%" PRIu32 " esp_mac_fail=%" PRIu32
                     " esp_raw_rx=%" PRIu32 " esp_wrong_src=%" PRIu32 " queue_drops=%" PRIu32,
                     status.device_id, channel, wifi_link_is_connected(), ethernet_link_is_connected(), active->name,
                     status.tx_count, status.rx_count, stats.mac_tx_ok, stats.mac_tx_fail,
                     stats.raw_rx, stats.rejected_source, snapshot.queue_drops);
        }
        if (now >= next_display) {
            next_display = now + 250;
            if (startup_complete) {
                display_update(status.device_id, active->name, status.peer_reachable,
                               status.tx_count, status.rx_count);
            } else {
                int64_t elapsed = now - startup_begin;
                display_connecting(status.device_id, (uint32_t)elapsed,
                                   elapsed >= LINK_STARTUP_WAIT_NOTICE_MS);
            }
        }
    }
}
esp_err_t transport_start(void)
{
    if (receive_queue) return ESP_ERR_INVALID_STATE;
    uint8_t local_mac[6];
    esp_err_t err = esp_read_mac(local_mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) return err;
    const uint8_t *peer_mac;
    if (memcmp(local_mac, MAC_A, 6) == 0) {
        status.device_id = 'A'; peer_id = 'B'; peer_mac = MAC_B;
    } else if (memcmp(local_mac, MAC_B, 6) == 0) {
        status.device_id = 'B'; peer_id = 'A'; peer_mac = MAC_A;
    } else {
        ESP_LOGE(TAG, "Unknown board MAC " MACSTR, MAC2STR(local_mac));
        return ESP_ERR_NOT_FOUND;
    }
    display_connecting(status.device_id, 0, false);
    receive_queue = xQueueCreate(32, sizeof(receive_event_t));
    if (!receive_queue) return ESP_ERR_NO_MEM;
    status.active = LINK_ESPNOW;
    publish();
    err = wifi_link_init(status.device_id, on_receive);
    if (err != ESP_OK) return err;
    session = esp_random();
    err = espnow_link_init(peer_mac, on_receive);
    if (err != ESP_OK) ESP_LOGE(TAG, "ESP-NOW init failed: %s; UDP still available", esp_err_to_name(err));
    if (LINK_ETH_ENABLED) {
        err = ethernet_link_init(status.device_id, on_receive);
        if (err != ESP_OK) ESP_LOGE(TAG, "Ethernet unavailable: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Board %c MAC " MACSTR ", peer %c MAC " MACSTR ", session %" PRIu32
             ", protocol=%u packet_size=%u", status.device_id, MAC2STR(local_mac), peer_id,
             MAC2STR(peer_mac), session, LINK_VERSION, (unsigned)sizeof(link_packet_t));
    return xTaskCreate(transport_task, "transport", 6144, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
