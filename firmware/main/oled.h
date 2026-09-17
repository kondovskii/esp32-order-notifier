#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64

// Sets up SPI, resets the display, and sends the SSD1309 init sequence.
esp_err_t oled_init(void);

// Drawing happens in RAM (a framebuffer). Nothing appears until oled_flush().
void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
void oled_hline(int x, int y, int w);

// Text uses a 5x7 font. scale 1 = 7 px tall, scale 2 = 14 px tall, etc.
// on = true draws lit pixels; false draws dark pixels (for text on a filled box).
void oled_text(int x, int y, const char *s, int scale, bool on);
void oled_text_centered(int y, const char *s, int scale, bool on);
int oled_text_width(const char *s, int scale);

// Sends the whole framebuffer to the display.
esp_err_t oled_flush(void);