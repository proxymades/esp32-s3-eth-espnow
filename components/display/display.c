#include "display.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

/* ============================================================
   TFT CONFIG
   ============================================================ */

#define LCD_HOST SPI3_HOST

#define TFT_SCLK 16
#define TFT_MOSI 17
#define TFT_CS 18
#define TFT_DC 15
#define TFT_RST 38

#define TFT_WIDTH 240
#define TFT_HEIGHT 320

/* ============================================================
   COLORS RGB565
   ============================================================ */

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_GREEN 0x07E0
#define COLOR_PURPLE 0xF81F
/* uint16_t framebuffer is little-endian; ST7789 consumes big-endian SPI bytes. */
#define COLOR_RED 0x00F8  /* RGB565 red 0xF800, transmitted as F8 00 */
#define COLOR_BLUE 0x1F00 /* RGB565 blue 0x001F, transmitted as 00 1F */
#define COLOR_RX_YELLOW_ORANGE 0x00FE /* RGB565 golden yellow 0xFE00, SPI FE 00 */
#define COLOR_ACTIVITY_IDLE 0x0842 /* RGB565 dark gray 0x4208, SPI 42 08 */

static esp_lcd_panel_handle_t panel = NULL;

static uint16_t *framebuffer = NULL;

/* ============================================================
   FONT 5x7
   ============================================================ */

static void glyph(
    char c,
    uint8_t g[5])
{
    memset(
        g,
        0,
        5);

    switch (c)
    {
    case 'A':
    {
        uint8_t a[5] =
            {0x7E, 0x11, 0x11, 0x11, 0x7E};

        memcpy(g, a, 5);
        break;
    }

    case 'B':
    {
        uint8_t a[5] =
            {0x7F, 0x49, 0x49, 0x49, 0x36};

        memcpy(g, a, 5);
        break;
    }

    case 'C':
    {
        uint8_t a[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
        memcpy(g, a, 5);
        break;
    }

    case 'G':
    {
        uint8_t a[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
        memcpy(g, a, 5);
        break;
    }

    case 'E':
    {
        uint8_t a[5] =
            {0x7F, 0x49, 0x49, 0x49, 0x41};

        memcpy(g, a, 5);
        break;
    }

    case 'I':
    {
        uint8_t a[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
        memcpy(g, a, 5);
        break;
    }
    case 'F':
    {
        uint8_t a[5] = {0x7F, 0x09, 0x09, 0x09, 0x01};
        memcpy(g, a, 5);
        break;
    }

    case 'H':
    {
        uint8_t a[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
        memcpy(g, a, 5);
        break;
    }

    case 'L':
    {
        uint8_t a[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
        memcpy(g, a, 5);
        break;
    }

    case 'K':
    {
        uint8_t a[5] = {0x7F, 0x08, 0x14, 0x22, 0x41};
        memcpy(g, a, 5);
        break;
    }

    case 'N':
    {
        uint8_t a[5] =
            {0x7F, 0x02, 0x04, 0x08, 0x7F};

        memcpy(g, a, 5);
        break;
    }

    case 'O':
    {
        uint8_t a[5] =
            {0x3E, 0x41, 0x41, 0x41, 0x3E};

        memcpy(g, a, 5);
        break;
    }

    case 'P':
    {
        uint8_t a[5] =
            {0x7F, 0x09, 0x09, 0x09, 0x06};

        memcpy(g, a, 5);
        break;
    }

    case 'R':
    {
        uint8_t a[5] =
            {0x7F, 0x09, 0x19, 0x29, 0x46};

        memcpy(g, a, 5);
        break;
    }

    case 'S':
    {
        uint8_t a[5] =
            {0x46, 0x49, 0x49, 0x49, 0x31};

        memcpy(g, a, 5);
        break;
    }

    case 'T':
    {
        uint8_t a[5] =
            {0x01, 0x01, 0x7F, 0x01, 0x01};

        memcpy(g, a, 5);
        break;
    }

    case 'W':
    {
        uint8_t a[5] =
            {0x7F, 0x20, 0x18, 0x20, 0x7F};

        memcpy(g, a, 5);
        break;
    }

    case 'X':
    {
        uint8_t a[5] =
            {0x63, 0x14, 0x08, 0x14, 0x63};

        memcpy(g, a, 5);
        break;
    }

    case '-':
    {
        uint8_t a[5] =
            {0x08, 0x08, 0x08, 0x08, 0x08};

        memcpy(g, a, 5);
        break;
    }

    case ':':
    {
        uint8_t a[5] =
            {0x00, 0x36, 0x36, 0x00, 0x00};

        memcpy(g, a, 5);
        break;
    }

    case '0':
    {
        uint8_t a[5] =
            {0x3E, 0x51, 0x49, 0x45, 0x3E};

        memcpy(g, a, 5);
        break;
    }

    case '1':
    {
        uint8_t a[5] =
            {0x00, 0x42, 0x7F, 0x40, 0x00};

        memcpy(g, a, 5);
        break;
    }

    case '2':
    {
        uint8_t a[5] =
            {0x42, 0x61, 0x51, 0x49, 0x46};

        memcpy(g, a, 5);
        break;
    }

    case '3':
    {
        uint8_t a[5] =
            {0x21, 0x41, 0x45, 0x4B, 0x31};

        memcpy(g, a, 5);
        break;
    }

    case '4':
    {
        uint8_t a[5] =
            {0x18, 0x14, 0x12, 0x7F, 0x10};

        memcpy(g, a, 5);
        break;
    }

    case '5':
    {
        uint8_t a[5] =
            {0x27, 0x45, 0x45, 0x45, 0x39};

        memcpy(g, a, 5);
        break;
    }

    case '6':
    {
        uint8_t a[5] =
            {0x3C, 0x4A, 0x49, 0x49, 0x30};

        memcpy(g, a, 5);
        break;
    }

    case '7':
    {
        uint8_t a[5] =
            {0x01, 0x71, 0x09, 0x05, 0x03};

        memcpy(g, a, 5);
        break;
    }

    case '8':
    {
        uint8_t a[5] =
            {0x36, 0x49, 0x49, 0x49, 0x36};

        memcpy(g, a, 5);
        break;
    }

    case '9':
    {
        uint8_t a[5] =
            {0x06, 0x49, 0x49, 0x29, 0x1E};

        memcpy(g, a, 5);
        break;
    }

    default:
        break;
    }
}

/* ============================================================
   GRAPHICS
   ============================================================ */

static void pixel(
    int x,
    int y,
    uint16_t color)
{
    if (
        x < 0 ||
        y < 0 ||
        x >= TFT_WIDTH ||
        y >= TFT_HEIGHT)
    {
        return;
    }

    framebuffer[y * TFT_WIDTH + x] = color;
}

static void fill(
    uint16_t color)
{
    for (
        int i = 0;
        i < TFT_WIDTH * TFT_HEIGHT;
        i++)
    {
        framebuffer[i] = color;
    }
}

static void draw_char(
    int x,
    int y,
    char c,
    int scale,
    uint16_t color)
{
    uint8_t g[5];

    glyph(c, g);

    for (int col = 0; col < 5; col++)
    {
        for (int row = 0; row < 7; row++)
        {
            if (
                g[col] &
                (1 << row))
            {
                for (
                    int sx = 0;
                    sx < scale;
                    sx++)
                {
                    for (
                        int sy = 0;
                        sy < scale;
                        sy++)
                    {
                        pixel(
                            x +
                                col * scale +
                                sx,

                            y +
                                row * scale +
                                sy,

                            color);
                    }
                }
            }
        }
    }
}

static int text_width(
    const char *text,
    int scale)
{
    return strlen(text) *
           6 *
           scale;
}

static void draw_text(
    int x,
    int y,
    const char *text,
    int scale,
    uint16_t color)
{
    while (*text)
    {
        draw_char(
            x,
            y,
            *text,
            scale,
            color);

        x +=
            6 *
            scale;

        text++;
    }
}

static void draw_text_center(
    int y,
    const char *text,
    int scale,
    uint16_t color)
{
    int width =
        text_width(
            text,
            scale);

    draw_text(
        (
            TFT_WIDTH -
            width) /
            2,
        y,
        text,
        scale,
        color);
}

static void flush(void)
{
    ESP_ERROR_CHECK(
        esp_lcd_panel_draw_bitmap(
            panel,
            0,
            0,
            TFT_WIDTH,
            TFT_HEIGHT,
            framebuffer));
}

/* ============================================================
   INIT
   ============================================================ */

void display_init(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = TFT_SCLK,
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        .max_transfer_sz =
            TFT_WIDTH *
            TFT_HEIGHT *
            sizeof(uint16_t),
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            LCD_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = TFT_DC,
        .cs_gpio_num = TFT_CS,

        .pclk_hz =
            20 *
            1000 *
            1000,

        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,

        .spi_mode = 0,

        .trans_queue_depth = 1,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)
                LCD_HOST,

            &io_config,
            &io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_RST,

        .rgb_ele_order =
            LCD_RGB_ELEMENT_ORDER_RGB,

        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_st7789(
            io,
            &panel_config,
            &panel));

    ESP_ERROR_CHECK(
        esp_lcd_panel_reset(
            panel));

    ESP_ERROR_CHECK(
        esp_lcd_panel_init(
            panel));

    ESP_ERROR_CHECK(
        esp_lcd_panel_invert_color(panel, true));

    ESP_ERROR_CHECK(
        esp_lcd_panel_disp_on_off(
            panel,
            true));

    framebuffer =
        heap_caps_malloc(
            TFT_WIDTH *
                TFT_HEIGHT *
                sizeof(uint16_t),

            MALLOC_CAP_DMA |
                MALLOC_CAP_INTERNAL);

    ESP_ERROR_CHECK(
        framebuffer
            ? ESP_OK
            : ESP_ERR_NO_MEM);

    /* Show the startup screen before network initialization begins. */
    display_connecting(0, 0, false);
}

/* ============================================================
   STARTUP SCREEN
   ============================================================ */

void display_connecting(char device_id, uint32_t elapsed_ms, bool waiting_peer)
{
    fill(COLOR_BLACK);
    if (device_id == 'A' || device_id == 'B') {
        char device_text[2] = {device_id, '\0'};
        draw_text_center(20, device_text, 8, COLOR_WHITE);
    } else {
        draw_text_center(35, "ESP32-S3", 3, COLOR_WHITE);
    }

    draw_text_center(125, waiting_peer ? "WAITING" : "CONNECTING", 3, COLOR_GREEN);
    if (waiting_peer) draw_text_center(160, "PEER", 3, COLOR_WHITE);

    /* Three animated dots, drawn with existing framebuffer primitives. */
    unsigned illuminated = (elapsed_ms / 500) % 3 + 1;
    for (unsigned dot = 0; dot < 3; ++dot) {
        int center_x = TFT_WIDTH / 2 + ((int)dot - 1) * 24;
        uint16_t color = dot < illuminated ? COLOR_GREEN : COLOR_PURPLE;
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                if (dx * dx + dy * dy <= 16) pixel(center_x + dx, 205 + dy, color);
            }
        }
    }
    flush();
}

/* ============================================================
   DATA ACTIVITY (owned by the display caller task)
   ============================================================ */

typedef struct {
    bool initialized;
    uint32_t tx_count, rx_count;
    bool tx_pulse, rx_pulse;
    int64_t tx_started_us, rx_started_us;
} display_activity_t;

static display_activity_t activity;
#define ACTIVITY_PULSE_US 750000

static void draw_activity_arrow(int x, int y, bool sending, bool pulse, int64_t elapsed_us,
                                uint16_t active_color)
{
    bool lit = pulse && elapsed_us < ACTIVITY_PULSE_US;
    int offset = lit ? (int)(elapsed_us / 250000) * 3 : 6;
    uint16_t color = lit ? active_color : COLOR_ACTIVITY_IDLE;

    /* Right-moving TX; mirror both the arrow and its motion for left-moving RX. */
    for (int u = 0; u < 18; ++u) {
        for (int v = -6; v <= 6; ++v) {
            int abs_v = v < 0 ? -v : v;
            bool shaft = u <= 11 && abs_v <= 1;
            bool head = u >= 11 && abs_v <= 17 - u;
            if (shaft || head) {
                int local_x = offset + u;
                pixel(x + (sending ? local_x : 31 - local_x), y + 10 + v, color);
            }
        }
    }
}

static void draw_activity_counter(int y, const char *text, uint16_t color,
                                  bool sending, bool pulse, int64_t elapsed_us)
{
    const int arrow_width = 32, gap = 8;
    int scale = 3;
    /* Keep even ten-digit uint32 counters and arrows inside the 240px display. */
    if (text_width(text, scale) + arrow_width + gap > TFT_WIDTH - 24) scale = 2;
    int width = text_width(text, scale) + arrow_width + gap;
    int x = (TFT_WIDTH - width) / 2;
    draw_text(x, y + (21 - 7 * scale) / 2, text, scale, color);
    draw_activity_arrow(x + text_width(text, scale) + gap, y,
                        sending, pulse, elapsed_us, color);
}

/* ============================================================
   STATUS SCREEN
   ============================================================ */

void display_update(
    char device_id,
    const char *transport_name,
    bool peer_reachable,
    uint32_t tx_count,
    uint32_t rx_count)
{
    char text[32];
    int64_t now_us = esp_timer_get_time();
    if (!activity.initialized) {
        /* Startup traffic already counted is not a new activity event. */
        activity.initialized = true;
        activity.tx_count = tx_count;
        activity.rx_count = rx_count;
    }
    if (tx_count != activity.tx_count) {
        activity.tx_count = tx_count;
        activity.tx_started_us = now_us;
        activity.tx_pulse = true;
    }
    if (rx_count != activity.rx_count) {
        activity.rx_count = rx_count;
        activity.rx_started_us = now_us;
        activity.rx_pulse = true;
    }

    fill(
        COLOR_BLACK);

    /* DEVICE A / B */

    char device_text[2] = {
        device_id,
        '\0'};

    draw_text_center(
        20,
        device_text,
        8,
        COLOR_WHITE);

    /* Name a transport only while end-to-end exchange is verified. */
    if (peer_reachable) {
        draw_text_center(120, transport_name, 4, COLOR_BLUE);
    } else {
        draw_text_center(120, "NO LINK", 4, COLOR_RED);
    }

    /* TX */

    snprintf(
        text,
        sizeof(text),
        "TX:%lu",
        (unsigned long)
            tx_count);

    draw_activity_counter(205, text, COLOR_PURPLE, true, activity.tx_pulse,
                          now_us - activity.tx_started_us);

    /* RX */

    snprintf(
        text,
        sizeof(text),
        "RX:%lu",
        (unsigned long)
            rx_count);

    draw_activity_counter(255, text, COLOR_RX_YELLOW_ORANGE, false, activity.rx_pulse,
                          now_us - activity.rx_started_us);

    flush();
}
