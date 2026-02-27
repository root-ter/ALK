#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../base/mb2/mb2.h"

// Цвета (RGB формат)
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} color_t;

// Константы цветов
#define COLOR_BLACK      ((color_t){0, 0, 0})
#define COLOR_WHITE      ((color_t){255, 255, 255})
#define COLOR_RED        ((color_t){255, 0, 0})
#define COLOR_GREEN      ((color_t){0, 255, 0})
#define COLOR_BLUE       ((color_t){0, 0, 255})
#define COLOR_YELLOW     ((color_t){255, 255, 0})
#define COLOR_CYAN       ((color_t){0, 255, 255})
#define COLOR_MAGENTA    ((color_t){255, 0, 255})
#define COLOR_GRAY       ((color_t){128, 128, 128})
#define COLOR_DARK_GRAY  ((color_t){64, 64, 64})
#define COLOR_LIGHT_GRAY ((color_t){192, 192, 192})

// Размеры шрифта
#define FONT_WIDTH   8
#define FONT_HEIGHT  12
#define FONT_SPACING 1

// Контекст фреймбуфера
typedef struct {
    uint8_t* buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint32_t pixel_size;
    
    // Текущие параметры
    uint32_t cursor_x;
    uint32_t cursor_y;
    color_t fg_color;
    color_t bg_color;
    uint32_t tab_size;
    
    // Информация о цветах (стандартные маски для разных BPP)
    struct {
        uint32_t red_mask;
        uint32_t green_mask;
        uint32_t blue_mask;
        uint8_t red_shift;
        uint8_t green_shift;
        uint8_t blue_shift;
    } rgb;
} framebuffer_t;

// Инициализация фреймбуфера
bool fb_init(framebuffer_t* fb);

// Базовые операции рисования
void fb_clear(framebuffer_t* fb, color_t color);
void fb_set_pixel(framebuffer_t* fb, uint32_t x, uint32_t y, color_t color);
color_t fb_get_pixel(framebuffer_t* fb, uint32_t x, uint32_t y);

// Рисование фигур
void fb_draw_line(framebuffer_t* fb, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, color_t color);
void fb_draw_rect(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color);
void fb_fill_rect(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color);
void fb_draw_circle(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t radius, color_t color);
void fb_fill_circle(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t radius, color_t color);

// Работа с текстом
void fb_set_color(framebuffer_t* fb, color_t fg, color_t bg);
void fb_set_cursor(framebuffer_t* fb, uint32_t x, uint32_t y);
void fb_put_char(framebuffer_t* fb, char c);
void fb_print(framebuffer_t* fb, const char* str);
void fb_printf(framebuffer_t* fb, const char* fmt, ...);

// Полезные функции
uint32_t fb_rgb_to_pixel(framebuffer_t* fb, color_t color);
void fb_scroll(framebuffer_t* fb, uint32_t lines);
void fb_newline(framebuffer_t* fb);

#endif // FRAMEBUFFER_H
