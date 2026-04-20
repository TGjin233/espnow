#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_lcd_st7789.h"
#include "driver/spi_master.h"
#include "esp_log.h"


#define LCD_HOST SPI2_HOST
#define LCD_V_RES 320
#define LCD_H_RES 240
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_CS 5
#define LCD_DC 2
#define LCD_RST 4
#define LCD_BL 17
#define LCD_MOSI 23
#define LCD_CLK 18

#define LCD_BG_COLOR 0x0000
#define LCD_TEXT_COLOR 0xFFFF
#define LCD_ERROR_COLOR 0xF800

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t bg_color;
    uint16_t text_color;
} lcd_context_t;

typedef enum {
    LCD_ALIGN_LEFT,
    LCD_ALIGN_CENTER,
    LCD_ALIGN_RIGHT
} lcd_text_align_t;

esp_err_t lcd_init(void);
void lcd_set_backlight(bool on);
void lcd_clear(uint16_t color);
void lcd_fill_rect(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end, uint16_t color);
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void lcd_draw_rect(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end, uint16_t color);
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(uint16_t x_center, uint16_t y_center, uint16_t radius, uint16_t color);
void lcd_fill_circle(uint16_t x_center, uint16_t y_center, uint16_t radius, uint16_t color);
void lcd_draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg_color, uint16_t bg_color, uint8_t size);
void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg_color, uint16_t bg_color, uint8_t size, lcd_text_align_t align);
void lcd_set_bg_color(uint16_t color);
void lcd_set_text_color(uint16_t color);
void lcd_draw_progress_bar(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t percentage, uint16_t fg_color, uint16_t bg_color);

void lcd_display_task(void *pvParameter);

#endif
