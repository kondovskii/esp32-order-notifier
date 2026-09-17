#include <string.h>
#include "oled.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

// Wiring (see README). SCK and MOSI use the ESP32's native VSPI pins.
#define PIN_SCK  18
#define PIN_MOSI 23
#define PIN_RES  19
#define PIN_DC   16
#define PIN_CS   17

#define SPI_HOST_ID  SPI3_HOST     // "VSPI"
#define SPI_SPEED_HZ 4000000       // 4 MHz: well under the SSD1309's limit, safe on a breadboard

#define PAGES (OLED_HEIGHT / 8)    // the display RAM is 8 horizontal strips ("pages")

static const char *TAG = "oled";
static spi_device_handle_t s_spi;

// One byte per column per page. Bit 0 of a byte = top pixel of that page.
static uint8_t s_fb[PAGES][OLED_WIDTH];

// The DC pin tells the display whether bytes are commands (low) or pixel data (high).
static esp_err_t spi_send(bool is_data, const uint8_t *buf, size_t len)
{
    gpio_set_level(PIN_DC, is_data ? 1 : 0);
    spi_transaction_t t = {
        .length = len * 8,  // length is in bits
        .tx_buffer = buf,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

esp_err_t oled_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_RES) | (1ULL << PIN_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,       // the display never talks back
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OLED_WIDTH,
    };
    esp_err_t err = spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev = {
        .mode = 0,                  // clock idles low, data sampled on rising edge
        .clock_speed_hz = SPI_SPEED_HZ,
        .spics_io_num = PIN_CS,     // the driver pulls CS low around each transfer
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI_HOST_ID, &dev, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    // Hardware reset: hold RES low briefly, then release.
    gpio_set_level(PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    static const uint8_t init_cmds[] = {
        0xAE,        // display off while configuring
        0xD5, 0xA0,  // clock divide ratio / oscillator frequency
        0xA8, 0x3F,  // multiplex ratio: 64 rows
        0xD3, 0x00,  // no vertical offset
        0x40,        // display start line 0
        0xA1,        // flip columns so x=0 is on the left
        0xC8,        // flip rows so y=0 is at the top
        0xDA, 0x12,  // COM pin layout for 128x64 panels
        0x81, 0x8F,  // contrast (brightness), 0x00-0xFF
        0xD9, 0xF1,  // pre-charge period
        0xDB, 0x34,  // VCOMH deselect level
        0xA4,        // show RAM contents (not all pixels on)
        0xA6,        // normal (not inverted)
    };
    err = spi_send(false, init_cmds, sizeof(init_cmds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Init sequence failed: %s", esp_err_to_name(err));
        return err;
    }

    oled_clear();
    oled_flush();

    static const uint8_t on_cmd[] = {0xAF};  // display on
    err = spi_send(false, on_cmd, sizeof(on_cmd));
    ESP_LOGI(TAG, "SSD1309 initialized");
    return err;
}

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;  // silently clip anything off-screen
    }
    uint8_t mask = 1 << (y % 8);
    if (on) {
        s_fb[y / 8][x] |= mask;
    } else {
        s_fb[y / 8][x] &= ~mask;
    }
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            oled_pixel(i, j, on);
        }
    }
}

void oled_hline(int x, int y, int w)
{
    oled_fill_rect(x, y, w, 1, true);
}

static void draw_char(int x, int y, char c, int scale, bool on)
{
    if (c < 32 || c > 126) {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (glyph[col] & (1 << row)) {
                oled_fill_rect(x + col * scale, y + row * scale, scale, scale, on);
            }
        }
    }
}

int oled_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n == 0 ? 0 : (n * 6 - 1) * scale;  // 5 px glyph + 1 px gap, no gap after the last
}

void oled_text(int x, int y, const char *s, int scale, bool on)
{
    for (; *s; s++) {
        draw_char(x, y, *s, scale, on);
        x += 6 * scale;
    }
}

void oled_text_centered(int y, const char *s, int scale, bool on)
{
    oled_text((OLED_WIDTH - oled_text_width(s, scale)) / 2, y, s, scale, on);
}

esp_err_t oled_flush(void)
{
    for (int page = 0; page < PAGES; page++) {
        // Point the display's write cursor at column 0 of this page.
        uint8_t cmds[] = {
            0xB0 | page,  // page number
            0x00,         // column start, low nibble
            0x10,         // column start, high nibble
        };
        esp_err_t err = spi_send(false, cmds, sizeof(cmds));
        if (err == ESP_OK) {
            err = spi_send(true, s_fb[page], OLED_WIDTH);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}