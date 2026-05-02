#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c.h"

#define SSD1306_I2C_ADDR    0x3C

typedef struct {
    i2c_port_t port;
    uint8_t fb[CONFIG_SSD1306_WIDTH * CONFIG_SSD1306_HEIGHT / 8];
} ssd1306_t;

// Initialization: port I2C, pins SDA/SCL, frequency
esp_err_t ssd1306_init(ssd1306_t *display, i2c_port_t port, int sda, int scl, uint32_t clk_hz);

// Clear screen (full black)
void ssd1306_clear(ssd1306_t *display);

// Full screen (full white)
void ssd1306_fill(ssd1306_t *display);

// Draw pixel (x: 0..127, y: 0..63)
void ssd1306_draw_pixel(ssd1306_t *display, int x, int y, bool on);

// Draw horizontal line
void ssd1306_draw_hline(ssd1306_t *display, int x, int y, int len, bool on);

// Draw vertical line
void ssd1306_draw_vline(ssd1306_t *display, int x, int y, int len, bool on);

// Draw unfilled rectangle
void ssd1306_draw_rect(ssd1306_t *display, int x, int y, int w, int h, bool on);

// Draw filled rectangle
void ssd1306_fill_rect(ssd1306_t *display, int x, int y, int w, int h, bool on);

// Draw (print) character (internal shift 6×8, ASCII 32..127)
void ssd1306_draw_char(ssd1306_t *display, int x, int y, char c, bool on);

// Draw (print) string
void ssd1306_draw_string(ssd1306_t *display, int x, int y, const char *str, bool on);

// Send framebuffer to display
void ssd1306_refresh(const ssd1306_t *display);
