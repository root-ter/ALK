#include "fb.h"
#include "font/eng.h"
#include "font/num.h"
#include "font/sym.h"
#include <stdarg.h>
#include "../../libc/string.h"
#include "../../base/math.h"

// Вспомогательная функция для вычисления сдвигов масок
static void calculate_rgb_shifts(framebuffer_t* fb) {
    // Стандартные маски для разных BPP
    if (fb->bpp == 32) {
        fb->rgb.red_mask = 0x00FF0000;
        fb->rgb.green_mask = 0x0000FF00;
        fb->rgb.blue_mask = 0x000000FF;
        fb->rgb.red_shift = 16;
        fb->rgb.green_shift = 8;
        fb->rgb.blue_shift = 0;
    } else if (fb->bpp == 24) {
        fb->rgb.red_mask = 0xFF0000;
        fb->rgb.green_mask = 0x00FF00;
        fb->rgb.blue_mask = 0x0000FF;
        fb->rgb.red_shift = 16;
        fb->rgb.green_shift = 8;
        fb->rgb.blue_shift = 0;
    } else if (fb->bpp == 16) {
        // 5-6-5 формат
        fb->rgb.red_mask = 0xF800;
        fb->rgb.green_mask = 0x07E0;
        fb->rgb.blue_mask = 0x001F;
        fb->rgb.red_shift = 11;
        fb->rgb.green_shift = 5;
        fb->rgb.blue_shift = 0;
    } else if (fb->bpp == 8) {
        // Градации серого
        fb->rgb.red_mask = 0xFF;
        fb->rgb.green_mask = 0xFF;
        fb->rgb.blue_mask = 0xFF;
        fb->rgb.red_shift = 0;
        fb->rgb.green_shift = 0;
        fb->rgb.blue_shift = 0;
    }
}

// Функция для конвертации RGB в значение пикселя
static uint32_t rgb_to_pixel_32bit(framebuffer_t* fb, uint8_t r, uint8_t g, uint8_t b) {
    return ((r << fb->rgb.red_shift) & fb->rgb.red_mask) |
           ((g << fb->rgb.green_shift) & fb->rgb.green_mask) |
           ((b << fb->rgb.blue_shift) & fb->rgb.blue_mask);
}

static uint32_t rgb_to_pixel_24bit(framebuffer_t* fb, uint8_t r, uint8_t g, uint8_t b) {
    return ((r << fb->rgb.red_shift) & fb->rgb.red_mask) |
           ((g << fb->rgb.green_shift) & fb->rgb.green_mask) |
           ((b << fb->rgb.blue_shift) & fb->rgb.blue_mask);
}

static uint16_t rgb_to_pixel_16bit(framebuffer_t* fb, uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << fb->rgb.red_shift) |
                      ((g >> 2) << fb->rgb.green_shift) |
                      ((b >> 3) << fb->rgb.blue_shift));
}

// Инициализация фреймбуфера
bool fb_init(framebuffer_t* fb) {
    if (!fb) {
        return false;
    }
    
    // Получаем информацию о фреймбуфере из Multiboot2
    framebuffer_info_t* fbinfo = get_framebuffer_info();
    
    if (!fbinfo || fbinfo->addr == 0 || fbinfo->width == 0 || fbinfo->height == 0) {
        return false;
    }
    
    fb->buffer = (uint8_t*)(uintptr_t)fbinfo->addr;
    fb->width = fbinfo->width;
    fb->height = fbinfo->height;
    fb->pitch = fbinfo->pitch;
    fb->bpp = fbinfo->bpp;
    
    // Если pitch не указан, вычисляем его
    if (fb->pitch == 0) {
        fb->pitch = fb->width * ((fb->bpp + 7) / 8);
    }
    
    // Определяем размер пикселя в байтах
    fb->pixel_size = (fb->bpp + 7) / 8;
    
    // Рассчитываем сдвиги масок цветов
    calculate_rgb_shifts(fb);
    
    // Устанавливаем параметры по умолчанию
    fb->cursor_x = 0;
    fb->cursor_y = 0;
    fb->fg_color = COLOR_WHITE;
    fb->bg_color = COLOR_BLACK;
    fb->tab_size = 4;
    
    // Очищаем экран
    fb_clear(fb, fb->bg_color);
    
    return true;
}

// Очистка экрана
void fb_clear(framebuffer_t* fb, color_t color) {
    uint32_t pixel = fb_rgb_to_pixel(fb, color);
    
    if (fb->bpp == 32) {
        uint32_t* buffer = (uint32_t*)fb->buffer;
        for (uint32_t i = 0; i < fb->width * fb->height; i++) {
            buffer[i] = pixel;
        }
    } else if (fb->bpp == 24) {
        // Для 24-битного режима нужно работать с отдельными байтами
        for (uint32_t y = 0; y < fb->height; y++) {
            uint8_t* row = fb->buffer + y * fb->pitch;
            for (uint32_t x = 0; x < fb->width; x++) {
                uint8_t* pixel_ptr = row + x * 3;
                pixel_ptr[0] = color.b;
                pixel_ptr[1] = color.g;
                pixel_ptr[2] = color.r;
            }
        }
    } else if (fb->bpp == 16) {
        uint16_t* buffer = (uint16_t*)fb->buffer;
        for (uint32_t i = 0; i < fb->width * fb->height; i++) {
            buffer[i] = (uint16_t)pixel;
        }
    } else if (fb->bpp == 8) {
        uint8_t gray = (color.r * 30 + color.g * 59 + color.b * 11) / 100;
        memset(fb->buffer, gray, fb->height * fb->pitch);
    }
    
    fb->cursor_x = 0;
    fb->cursor_y = 0;
}

// Установка пикселя
void fb_set_pixel(framebuffer_t* fb, uint32_t x, uint32_t y, color_t color) {
    if (x >= fb->width || y >= fb->height) {
        return;
    }
    
    uint32_t pixel = fb_rgb_to_pixel(fb, color);
    uintptr_t offset = y * fb->pitch + x * fb->pixel_size;
    
    if (fb->bpp == 32) {
        *((uint32_t*)(fb->buffer + offset)) = pixel;
    } else if (fb->bpp == 24) {
        uint8_t* pixel_ptr = fb->buffer + offset;
        pixel_ptr[0] = color.b;
        pixel_ptr[1] = color.g;
        pixel_ptr[2] = color.r;
    } else if (fb->bpp == 16) {
        *((uint16_t*)(fb->buffer + offset)) = (uint16_t)pixel;
    } else if (fb->bpp == 8) {
        // Для 8-битного используем градации серого
        uint8_t gray = (color.r * 30 + color.g * 59 + color.b * 11) / 100;
        *((uint8_t*)(fb->buffer + offset)) = gray;
    }
}

// Получение цвета пикселя
color_t fb_get_pixel(framebuffer_t* fb, uint32_t x, uint32_t y) {
    if (x >= fb->width || y >= fb->height) {
        return COLOR_BLACK;
    }
    
    uintptr_t offset = y * fb->pitch + x * fb->pixel_size;
    
    if (fb->bpp == 32) {
        uint32_t pixel = *((uint32_t*)(fb->buffer + offset));
        return (color_t){
            (uint8_t)((pixel & fb->rgb.red_mask) >> fb->rgb.red_shift),
            (uint8_t)((pixel & fb->rgb.green_mask) >> fb->rgb.green_shift),
            (uint8_t)((pixel & fb->rgb.blue_mask) >> fb->rgb.blue_shift)
        };
    } else if (fb->bpp == 24) {
        uint8_t* pixel_ptr = fb->buffer + offset;
        return (color_t){pixel_ptr[2], pixel_ptr[1], pixel_ptr[0]};
    } else if (fb->bpp == 16) {
        uint16_t pixel = *((uint16_t*)(fb->buffer + offset));
        return (color_t){
            (uint8_t)(((pixel & fb->rgb.red_mask) >> fb->rgb.red_shift) << 3),
            (uint8_t)(((pixel & fb->rgb.green_mask) >> fb->rgb.green_shift) << 2),
            (uint8_t)(((pixel & fb->rgb.blue_mask) >> fb->rgb.blue_shift) << 3)
        };
    } else if (fb->bpp == 8) {
        uint8_t gray = *((uint8_t*)(fb->buffer + offset));
        return (color_t){gray, gray, gray};
    }
    
    return COLOR_BLACK;
}

// Рисование линии (алгоритм Брезенхэма) - БЕЗ ИЗМЕНЕНИЙ
void fb_draw_line(framebuffer_t* fb, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, color_t color) {
    int dx = abs((int)x2 - (int)x1);
    int dy = abs((int)y2 - (int)y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        fb_set_pixel(fb, x1, y1, color);
        
        if (x1 == x2 && y1 == y2) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

// Рисование прямоугольника - БЕЗ ИЗМЕНЕНИЙ
void fb_draw_rect(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color) {
    fb_draw_line(fb, x, y, x + w - 1, y, color);
    fb_draw_line(fb, x + w - 1, y, x + w - 1, y + h - 1, color);
    fb_draw_line(fb, x + w - 1, y + h - 1, x, y + h - 1, color);
    fb_draw_line(fb, x, y + h - 1, x, y, color);
}

// Заливка прямоугольника - БЕЗ ИЗМЕНЕНИЙ
void fb_fill_rect(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color) {
    for (uint32_t i = 0; i < h; i++) {
        fb_draw_line(fb, x, y + i, x + w - 1, y + i, color);
    }
}

// Рисование окружности - БЕЗ ИЗМЕНЕНИЙ
void fb_draw_circle(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t radius, color_t color) {
    int f = 1 - radius;
    int ddF_x = 0;
    int ddF_y = -2 * radius;
    int xi = 0;
    int yi = radius;
    
    fb_set_pixel(fb, x, y + radius, color);
    fb_set_pixel(fb, x, y - radius, color);
    fb_set_pixel(fb, x + radius, y, color);
    fb_set_pixel(fb, x - radius, y, color);
    
    while (xi < yi) {
        if (f >= 0) {
            yi--;
            ddF_y += 2;
            f += ddF_y;
        }
        xi++;
        ddF_x += 2;
        f += ddF_x + 1;
        
        fb_set_pixel(fb, x + xi, y + yi, color);
        fb_set_pixel(fb, x - xi, y + yi, color);
        fb_set_pixel(fb, x + xi, y - yi, color);
        fb_set_pixel(fb, x - xi, y - yi, color);
        fb_set_pixel(fb, x + yi, y + xi, color);
        fb_set_pixel(fb, x - yi, y + xi, color);
        fb_set_pixel(fb, x + yi, y - xi, color);
        fb_set_pixel(fb, x - yi, y - xi, color);
    }
}

// Заливка окружности - БЕЗ ИЗМЕНЕНИЙ
void fb_fill_circle(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t radius, color_t color) {
    for (uint32_t r = 0; r <= radius; r++) {
        fb_draw_circle(fb, x, y, r, color);
    }
}

// Установка цвета текста - БЕЗ ИЗМЕНЕНИЙ
void fb_set_color(framebuffer_t* fb, color_t fg, color_t bg) {
    fb->fg_color = fg;
    fb->bg_color = bg;
}

// Установка позиции курсора - БЕЗ ИЗМЕНЕНИЙ
void fb_set_cursor(framebuffer_t* fb, uint32_t x, uint32_t y) {
    fb->cursor_x = x;
    fb->cursor_y = y;
}

// Получение глифа для символа - БЕЗ ИЗМЕНЕНИЙ
static const uint8_t (*get_glyph(char c))[1] {
    switch (c) {
        // Заглавные буквы
        case 'A': return glyph_A;
        case 'B': return glyph_B;
        case 'C': return glyph_C;
        case 'D': return glyph_D;
        case 'E': return glyph_E;
        case 'F': return glyph_F;
        case 'G': return glyph_G;
        case 'H': return glyph_H;
        case 'I': return glyph_I;
        case 'J': return glyph_J;
        case 'K': return glyph_K;
        case 'L': return glyph_L;
        case 'M': return glyph_M;
        case 'N': return glyph_N;
        case 'O': return glyph_O;
        case 'P': return glyph_P;
        case 'Q': return glyph_Q;
        case 'R': return glyph_R;
        case 'S': return glyph_S;
        case 'T': return glyph_T;
        case 'U': return glyph_U;
        case 'V': return glyph_V;
        case 'W': return glyph_W;
        case 'X': return glyph_X;
        case 'Y': return glyph_Y;
        case 'Z': return glyph_Z;
        
        // Строчные буквы
        case 'a': return glyph_a;
        case 'b': return glyph_b;
        case 'c': return glyph_c;
        case 'd': return glyph_d;
        case 'e': return glyph_e;
        case 'f': return glyph_f;
        case 'g': return glyph_g;
        case 'h': return glyph_h;
        case 'i': return glyph_i;
        case 'j': return glyph_j;
        case 'k': return glyph_k;
        case 'l': return glyph_l;
        case 'm': return glyph_m;
        case 'n': return glyph_n;
        case 'o': return glyph_o;
        case 'p': return glyph_p;
        case 'q': return glyph_q;
        case 'r': return glyph_r;
        case 's': return glyph_s;
        case 't': return glyph_t;
        case 'u': return glyph_u;
        case 'v': return glyph_v;
        case 'w': return glyph_w;
        case 'x': return glyph_x;
        case 'y': return glyph_y;
        case 'z': return glyph_z;
        
        // Цифры
        case '0': return glyph_0;
        case '1': return glyph_1;
        case '2': return glyph_2;
        case '3': return glyph_3;
        case '4': return glyph_4;
        case '5': return glyph_5;
        case '6': return glyph_6;
        case '7': return glyph_7;
        case '8': return glyph_8;
        case '9': return glyph_9;
        
        // Символы
        case ' ': return glyph_space;
        case '!': return glyph_exclamation_mark;
        case '?': return glyph_question_mark;
        case '$': return glyph_dollar;
        case ':': return glyph_colon;
        case '.': return glyph_dot;
        case '_': return glyph_underscore;
        case '~': return glyph_tilde;
        case '@': return glyph_at;
        case '#': return glyph_hash;
        case '%': return glyph_percent;
        case '^': return glyph_caret;
        case '*': return glyph_asterisk;
        case '(': return glyph_left_parenthesis;
        case ')': return glyph_right_parenthesis;
        case '-': return glyph_minus;
        case '=': return glyph_equals;
        case '+': return glyph_plus;
        case '[': return glyph_left_square_bracket;
        case ']': return glyph_right_square_bracket;
        case '{': return glyph_left_curly_brace;
        case '}': return glyph_right_curly_brace;
        case ';': return glyph_semicolon;
        case ',': return glyph_comma;
        case '\'': return glyph_single_quote;
        case '"': return glyph_double_quote;
        case '<': return glyph_left_angle_bracket;
        case '>': return glyph_right_angle_bracket;
        case '/': return glyph_slash;
        case '\\': return glyph_backslash;
        case '|': return glyph_pipe;
        case '`': return glyph_backtick;
        case '&': return glyph_ampersand;
        
        default: return glyph_space;
    }
}

// Вывод символа - БЕЗ ИЗМЕНЕНИЙ
void fb_put_char(framebuffer_t* fb, char c) {
    // Обработка специальных символов
    if (c == '\n') {
        fb_newline(fb);
        return;
    }
    
    if (c == '\t') {
        uint32_t spaces = fb->tab_size - (fb->cursor_x / (FONT_WIDTH + FONT_SPACING)) % fb->tab_size;
        for (uint32_t i = 0; i < spaces; i++) {
            fb_put_char(fb, ' ');
        }
        return;
    }
    
    if (c == '\r') {
        fb->cursor_x = 0;
        return;
    }
    
    // Проверка на выход за границы экрана
    if (fb->cursor_y + FONT_HEIGHT >= fb->height) {
        fb_scroll(fb, FONT_HEIGHT + FONT_SPACING);
    }
    
    if (fb->cursor_x + FONT_WIDTH >= fb->width) {
        fb_newline(fb);
    }
    
    // Получаем глиф
    const uint8_t (*glyph)[1] = get_glyph(c);
    
    // Рисуем символ
    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t line = (*glyph)[row];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            uint32_t x = fb->cursor_x + col;
            uint32_t y = fb->cursor_y + row;
            
            if (line & (1 << (7 - col))) {
                fb_set_pixel(fb, x, y, fb->fg_color);
            } else {
                fb_set_pixel(fb, x, y, fb->bg_color);
            }
        }
    }
    
    fb->cursor_x += FONT_WIDTH + FONT_SPACING;
}

// Вывод строки - БЕЗ ИЗМЕНЕНИЙ
void fb_print(framebuffer_t* fb, const char* str) {
    while (*str) {
        fb_put_char(fb, *str++);
    }
}

// Простая реализация printf - БЕЗ ИЗМЕНЕНИЙ
void fb_printf(framebuffer_t* fb, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    char buffer[256];
    char* ptr = buffer;
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    // Простой перевод в строку
                    char num_buffer[32];
                    char* num_ptr = num_buffer + 31;
                    *num_ptr = '\0';
                    
                    int is_negative = 0;
                    if (val < 0) {
                        is_negative = 1;
                        val = -val;
                    }
                    
                    do {
                        *--num_ptr = '0' + (val % 10);
                        val /= 10;
                    } while (val > 0);
                    
                    if (is_negative) {
                        *--num_ptr = '-';
                    }
                    
                    fb_print(fb, num_ptr);
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    char num_buffer[32];
                    char* num_ptr = num_buffer + 31;
                    *num_ptr = '\0';
                    
                    do {
                        *--num_ptr = '0' + (val % 10);
                        val /= 10;
                    } while (val > 0);
                    
                    fb_print(fb, num_ptr);
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned int val = va_arg(args, unsigned int);
                    char num_buffer[32];
                    char* num_ptr = num_buffer + 31;
                    *num_ptr = '\0';
                    
                    const char* hex_digits = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
                    
                    do {
                        *--num_ptr = hex_digits[val & 0xF];
                        val >>= 4;
                    } while (val > 0);
                    
                    *--num_ptr = '0';
                    *--num_ptr = 'x';
                    
                    fb_print(fb, num_ptr);
                    break;
                }
                case 's': {
                    char* str = va_arg(args, char*);
                    fb_print(fb, str ? str : "(null)");
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    fb_put_char(fb, c);
                    break;
                }
                case '%': {
                    fb_put_char(fb, '%');
                    break;
                }
            }
        } else {
            fb_put_char(fb, *fmt);
        }
        fmt++;
    }
    
    va_end(args);
}

// Конвертация цвета в значение пикселя
uint32_t fb_rgb_to_pixel(framebuffer_t* fb, color_t color) {
    if (fb->bpp == 32) {
        return rgb_to_pixel_32bit(fb, color.r, color.g, color.b);
    } else if (fb->bpp == 24) {
        return rgb_to_pixel_24bit(fb, color.r, color.g, color.b);
    } else if (fb->bpp == 16) {
        return rgb_to_pixel_16bit(fb, color.r, color.g, color.b);
    } else if (fb->bpp == 8) {
        // Для 8-битного используем градации серого
        return (color.r * 30 + color.g * 59 + color.b * 11) / 100;
    }
    
    return 0;
}

// Прокрутка экрана - БЕЗ ИЗМЕНЕНИЙ
void fb_scroll(framebuffer_t* fb, uint32_t lines) {
    if (lines >= fb->height) {
        fb_clear(fb, fb->bg_color);
        return;
    }
    
    // Копируем содержимое экрана вверх
    uint32_t bytes_per_line = fb->pitch;
    uint32_t offset = lines * bytes_per_line;
    
    memmove(fb->buffer, fb->buffer + offset, (fb->height - lines) * bytes_per_line);
    
    // Очищаем освободившуюся область внизу
    for (uint32_t y = fb->height - lines; y < fb->height; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            fb_set_pixel(fb, x, y, fb->bg_color);
        }
    }
    
    // Обновляем позицию курсора
    if (fb->cursor_y >= lines) {
        fb->cursor_y -= lines;
    } else {
        fb->cursor_y = 0;
    }
}

// Новая строка - БЕЗ ИЗМЕНЕНИЙ
void fb_newline(framebuffer_t* fb) {
    fb->cursor_x = 0;
    fb->cursor_y += FONT_HEIGHT + FONT_SPACING;
    
    if (fb->cursor_y + FONT_HEIGHT >= fb->height) {
        fb_scroll(fb, FONT_HEIGHT + FONT_SPACING);
    }
}
