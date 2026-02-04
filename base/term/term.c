#include "term.h"
#include <stdarg.h>
#include "../../libc/string.h"
#include "../mem/mem.h"
#include <stdint.h>

// Константы
#define HISTORY_CAPACITY 1024  // Максимум 1024 строк в истории
#define INPUT_BUFFER_SIZE 256
#define DEFAULT_PROMPT "> "

// Вспомогательные функции для работы с историей
static char* get_history_line(term_t* term, uint32_t line_idx) {
    if (line_idx >= term->total_lines) return NULL;
    uint32_t idx = (term->history_start + line_idx) % term->history_size;
    return &term->history_buffer[idx * (term->cols + 1)];
}

static char* get_screen_line(term_t* term, uint32_t screen_line) {
    if (screen_line >= term->rows) return NULL;
    return &term->screen_buffer[screen_line * (term->cols + 1)];
}

static void add_history_line(term_t* term, const char* line) {
    if (term->total_lines >= term->history_size) {
        term->history_start = (term->history_start + 1) % term->history_size;
        term->total_lines--;
    }
    
    char* dest = &term->history_buffer[term->history_end * (term->cols + 1)];
    strncpy(dest, line, term->cols);
    dest[term->cols] = '\0';
    
    term->history_end = (term->history_end + 1) % term->history_size;
    term->total_lines++;
}

// ==================== ОТОБРАЖЕНИЕ ПРОМПТА ====================

static void draw_prompt_line(term_t* term) {
    if (!term->prompt_enabled || !term->fb) return;
    
    // Промпт всегда на последней видимой строке
    uint32_t prompt_line = term->rows - 1;
    uint32_t py = term->y + prompt_line * term->char_height;
    
    // Проверяем что строка промпта в пределах экрана
    if (py + term->char_height > term->fb->height) {
        // Слишком низко! Корректируем
        py = term->fb->height - term->char_height - 1;
        if (py < term->y) py = term->y; // Запасной вариант
    }
    
    // 1. Очищаем только строку промпта
    fb_fill_rect(term->fb,
                 term->x, py,
                 term->cols * term->char_width,
                 FONT_HEIGHT,
                 term->bg_color);
    
    // 2. Рисуем текст промпта
    fb_set_color(term->fb, term->fg_color, term->bg_color);
    fb_set_cursor(term->fb, term->x, py);
    fb_print(term->fb, term->prompt_text);
    
    // 3. Рисуем введённый текст
    uint32_t text_x = term->x + strlen(term->prompt_text) * term->char_width;
    fb_set_cursor(term->fb, text_x, py);
    
    for (uint32_t i = 0; i < term->input_pos && i < term->cols - strlen(term->prompt_text); i++) {
        fb_put_char(term->fb, term->input_buffer[i]);
    }
    
    // 4. Рисуем курсор
    uint32_t cursor_x = strlen(term->prompt_text) + term->input_cursor;
    if (cursor_x < term->cols) {
        uint32_t cursor_px = term->x + cursor_x * term->char_width;
        uint32_t cursor_py = py + FONT_HEIGHT - 2;
        
        // Проверяем границы курсора
        if (cursor_px < term->x + term->cols * term->char_width) {
            fb_fill_rect(term->fb,
                         cursor_px, cursor_py,
                         term->char_width, 2,
                         term->fg_color);
        }
    }
}

// ==================== ОБНОВЛЕНИЕ ЭКРАНА ====================

static void refresh_screen(term_t* term) {
    // Очищаем буфер экрана
    memset(term->screen_buffer, ' ', term->screen_size);
    
    // Заполняем видимые строки из истории
    for (uint32_t i = 0; i < term->rows - (term->prompt_enabled ? 1 : 0); i++) {
        int32_t history_idx = (int32_t)term->total_lines - 
                            (int32_t)term->scroll_offset - 
                            (term->rows - (term->prompt_enabled ? 1 : 0)) + i;
        
        if (history_idx >= 0 && history_idx < (int32_t)term->total_lines) {
            char* history_line = get_history_line(term, history_idx);
            char* screen_line = get_screen_line(term, i);
            if (history_line && screen_line) {
                strncpy(screen_line, history_line, term->cols);
            }
        }
    }
    
    // Отрисовываем текст
    for (uint32_t y = 0; y < term->rows - (term->prompt_enabled ? 1 : 0); y++) {
        uint32_t py = term->y + y * term->char_height;
        
        // Очищаем строку
        fb_fill_rect(term->fb,
                     term->x, py,
                     term->cols * term->char_width,
                     FONT_HEIGHT,
                     term->bg_color);
        
        // Рисуем текст строки
        char* line_text = get_screen_line(term, y);
        if (!line_text) continue;
        
        fb_set_color(term->fb, term->fg_color, term->bg_color);
        for (uint32_t x = 0; x < term->cols; x++) {
            char c = line_text[x];
            if (c == ' ') continue;
            
            uint32_t px = term->x + x * term->char_width;
            fb_set_cursor(term->fb, px, py);
            fb_put_char(term->fb, c);
        }
    }
    
    // Рисуем промпт
    if (term->prompt_enabled) {
        draw_prompt_line(term);
    }
    
    term->needs_redraw = false;
}

// ==================== ИНИЦИАЛИЗАЦИЯ ====================

term_t* term_init(framebuffer_t* fb, uint32_t x, uint32_t y, 
                  uint32_t cols, uint32_t rows) {
    term_t* term = malloc(sizeof(term_t));
    if (!term) return NULL;
    
    // Базовые параметры
    term->fb = fb;
    term->x = x;
    term->y = y;
    term->cols = cols;
    term->rows = rows;
    term->char_width = FONT_WIDTH + 1;
    term->char_height = FONT_HEIGHT + 2;
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->scroll_offset = 0;
    term->total_lines = 0;
    term->bg_color = (color_t){0, 0, 0};
    term->fg_color = (color_t){200, 200, 200};
    
    // История
    term->history_size = HISTORY_CAPACITY;
    term->history_start = 0;
    term->history_end = 0;
    term->history_buffer = malloc(term->history_size * (cols + 1));
    if (!term->history_buffer) {
        free(term);
        return NULL;
    }
    memset(term->history_buffer, 0, term->history_size * (cols + 1));
    
    // Экранный буфер
    term->screen_size = rows * (cols + 1);
    term->screen_buffer = malloc(term->screen_size);
    if (!term->screen_buffer) {
        free(term->history_buffer);
        free(term);
        return NULL;
    }
    memset(term->screen_buffer, ' ', term->screen_size);
    
    // Ввод
    term->prompt_enabled = false;
    term->input_buffer = malloc(INPUT_BUFFER_SIZE);
    if (!term->input_buffer) {
        free(term->screen_buffer);
        free(term->history_buffer);
        free(term);
        return NULL;
    }
    term->input_capacity = INPUT_BUFFER_SIZE - 1;
    term->input_pos = 0;
    term->input_cursor = 0;
    term->input_buffer[0] = '\0';
    strcpy(term->prompt_text, DEFAULT_PROMPT);
    
    term->needs_redraw = true;
    
    // Очищаем и рисуем начальное состояние
    term_clear(term);
    
    return term;
}

// ==================== ОСНОВНЫЕ ФУНКЦИИ ====================

void term_clear(term_t* term) {
    // Очищаем графический экран
    fb_fill_rect(term->fb, 
                 term->x, term->y,
                 term->cols * term->char_width,
                 term->rows * term->char_height,
                 term->bg_color);
    
    // Сбрасываем состояние
    term->history_start = 0;
    term->history_end = 0;
    term->total_lines = 0;
    term->scroll_offset = 0;
    term->input_pos = 0;
    term->input_cursor = 0;
    term->input_buffer[0] = '\0';
    
    memset(term->screen_buffer, ' ', term->screen_size);
    memset(term->history_buffer, 0, term->history_size * (term->cols + 1));
    
    term->needs_redraw = true;
    refresh_screen(term);
}

void term_putc(term_t* term, char c) {
    static char current_line[256];
    static uint32_t line_pos = 0;
    
    switch (c) {
        case '\n':
            // Завершаем текущую строку и добавляем в историю
            current_line[line_pos] = '\0';
            if (line_pos > 0) {
                add_history_line(term, current_line);
            }
            term->scroll_offset = 0;
            line_pos = 0;
            term->needs_redraw = true;
            break;
            
        case '\r':
            // Возврат каретки - сбрасываем позицию строки
            line_pos = 0;
            break;
            
        case '\t':
            // Табуляция
            do {
                if (line_pos >= term->cols - 1) break;
                current_line[line_pos++] = ' ';
            } while (line_pos % 4 != 0);
            break;
            
        case '\b':
            // Backspace
            if (line_pos > 0) {
                line_pos--;
            }
            break;
            
        default:
            // Обычный символ
            if (line_pos >= term->cols - 1) {
                // Строка переполнена - завершаем её
                current_line[line_pos] = '\0';
                add_history_line(term, current_line);
                term->scroll_offset = 0;
                line_pos = 0;
                term->needs_redraw = true;
            }
            
            if (c >= 32 && c < 127) {
                current_line[line_pos++] = c;
            }
            break;
    }
    
    if (term->needs_redraw) {
        refresh_screen(term);
    }
}

void term_puts(term_t* term, const char* str) {
    while (*str) {
        term_putc(term, *str++);
    }
}

// ==================== ПРОКРУТКА ====================

void term_scroll_up(term_t* term, uint32_t lines) {
    // Прокрутка вверх = показываем более старые строки
    uint32_t max_scroll = 0;
    if (term->total_lines > term->rows) {
        max_scroll = term->total_lines - term->rows;
    }
    
    if (term->scroll_offset + lines > max_scroll) {
        term->scroll_offset = max_scroll;
    } else {
        term->scroll_offset += lines;
    }
    
    term->needs_redraw = true;
    refresh_screen(term);
}

void term_scroll_down(term_t* term, uint32_t lines) {
    // Прокрутка вниз = показываем более новые строки
    if (term->scroll_offset < lines) {
        term->scroll_offset = 0;
    } else {
        term->scroll_offset -= lines;
    }
    
    term->needs_redraw = true;
    refresh_screen(term);
}

// ==================== УПРАВЛЕНИЕ ПРОМПТОМ ====================

void term_enable_prompt(term_t* term) {
    if (term->prompt_enabled) return;
    
    term->prompt_enabled = true;
    term->input_pos = 0;
    term->input_cursor = 0;
    term->input_buffer[0] = '\0';
    
    term->needs_redraw = true;
    refresh_screen(term);
}

void term_disable_prompt(term_t* term) {
    if (!term->prompt_enabled) return;
    
    term->prompt_enabled = false;
    term->needs_redraw = true;
    refresh_screen(term);
}

bool term_is_prompt_enabled(term_t* term) {
    return term->prompt_enabled;
}

void term_set_prompt_text(term_t* term, const char* text) {
    strncpy(term->prompt_text, text, sizeof(term->prompt_text) - 1);
    term->prompt_text[sizeof(term->prompt_text) - 1] = '\0';
    
    if (term->prompt_enabled) {
        term->needs_redraw = true;
        refresh_screen(term);
    }
}

void term_clear_input(term_t* term) {
    term->input_pos = 0;
    term->input_cursor = 0;
    term->input_buffer[0] = '\0';
    
    if (term->prompt_enabled) {
        term->needs_redraw = true;
        refresh_screen(term);
    }
}

// ==================== ОБРАБОТКА ВВОДА С КЛАВИАТУРЫ ====================

bool term_handle_input(term_t* term, char input_char, char** out_line) {
    if (!term->prompt_enabled) return false;
    if (out_line) *out_line = NULL;
    
    switch (input_char) {
        case '\n':  // Enter
        case '\r':
            if (term->input_pos > 0) {
                term->input_buffer[term->input_pos] = '\0';
                if (out_line) {
                    *out_line = term->input_buffer;
                }
                // Очищаем ввод после возврата
                term->input_pos = 0;
                term->input_cursor = 0;
                term->needs_redraw = true;
                refresh_screen(term);
                return true;
            }
            return false;
            
        case '\b':  // Backspace
            if (term->input_pos > 0 && term->input_cursor > 0) {
                // Сдвигаем символы влево
                for (uint32_t i = term->input_cursor - 1; i < term->input_pos; i++) {
                    term->input_buffer[i] = term->input_buffer[i + 1];
                }
                term->input_pos--;
                term->input_cursor--;
                term->needs_redraw = true;
                refresh_screen(term);
            }
            return false;
            
        case 0x03:  // Ctrl+C
            term_clear_input(term);
            return false;
            
        default:
            // Обычный символ
            if (input_char >= 32 && input_char < 127 && 
                term->input_pos < term->input_capacity) {
                
                // Сдвигаем символы вправо, если нужно
                if (term->input_cursor < term->input_pos) {
                    for (uint32_t i = term->input_pos; i > term->input_cursor; i--) {
                        term->input_buffer[i] = term->input_buffer[i - 1];
                    }
                }
                
                // Вставляем символ
                term->input_buffer[term->input_cursor] = input_char;
                term->input_pos++;
                term->input_cursor++;
                term->needs_redraw = true;
                refresh_screen(term);
            }
            return false;
    }
}

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

void term_set_color(term_t* term, color_t fg, color_t bg) {
    term->fg_color = fg;
    term->bg_color = bg;
    term->needs_redraw = true;
    refresh_screen(term);
}

void term_set_cursor(term_t* term, uint32_t x, uint32_t y) {
    // Эта функция теперь не влияет на графический курсор напрямую
    // так как мы используем собственное управление курсором
    term->cursor_x = x;
    term->cursor_y = y;
}

void term_get_size(term_t* term, uint32_t* cols, uint32_t* rows) {
    if (cols) *cols = term->cols;
    if (rows) *rows = term->rows;
}

void term_get_cursor(term_t* term, uint32_t* x, uint32_t* y) {
    if (x) *x = term->cursor_x;
    if (y) *y = term->cursor_y;
}

// ==================== ФОРМАТИРОВАННЫЙ ВЫВОД ====================

void term_printf(term_t* term, const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    
    char* ptr = buffer;
    const char* f = fmt;
    
    while (*f && (ptr - buffer) < 1023) {
        if (*f == '%') {
            f++;
            
            // Флаги форматирования
            int width = 0;
            int precision = -1;
            bool zero_pad = false;
            bool left_align = false;
            bool alternate = false;
            bool space = false;
            bool plus = false;
            
            // Обработка флагов
            while (1) {
                if (*f == '-') {
                    left_align = true;
                    f++;
                } else if (*f == '0') {
                    zero_pad = true;
                    f++;
                } else if (*f == '#') {
                    alternate = true;
                    f++;
                } else if (*f == ' ') {
                    space = true;
                    f++;
                } else if (*f == '+') {
                    plus = true;
                    f++;
                } else {
                    break;
                }
            }
            
            // Ширина
            while (*f >= '0' && *f <= '9') {
                width = width * 10 + (*f - '0');
                f++;
            }
            
            // Точность
            if (*f == '.') {
                f++;
                precision = 0;
                while (*f >= '0' && *f <= '9') {
                    precision = precision * 10 + (*f - '0');
                    f++;
                }
            }
            
            // Модификаторы размера
            bool is_long = false;
            bool is_long_long = false;
            bool is_size_t = false;
            bool is_short = false;
            bool is_char = false;
            
            if (*f == 'h') {
                f++;
                if (*f == 'h') {
                    is_char = true;
                    f++;
                } else {
                    is_short = true;
                }
            } else if (*f == 'l') {
                f++;
                if (*f == 'l') {
                    is_long_long = true;
                    f++;
                } else {
                    is_long = true;
                }
            } else if (*f == 'z') {
                is_size_t = true;
                f++;
            } else if (*f == 't') {
                // ptrdiff_t - обрабатываем как long
                is_long = true;
                f++;
            } else if (*f == 'j') {
                // intmax_t - обрабатываем как long long
                is_long_long = true;
                f++;
            }
            
            switch (*f) {
                case 'd':
                case 'i': {
                    long long val;
                    if (is_long_long) {
                        val = va_arg(args, long long);
                    } else if (is_long) {
                        val = va_arg(args, long);
                    } else if (is_size_t) {
                        val = (long long)va_arg(args, size_t);
                    } else if (is_short) {
                        val = (short)va_arg(args, int);
                    } else if (is_char) {
                        val = (signed char)va_arg(args, int);
                    } else {
                        val = va_arg(args, int);
                    }
                    
                    char num_buf[32];
                    char* num_ptr = num_buf + 31;
                    *num_ptr = '\0';
                    
                    if (val == 0) {
                        *--num_ptr = '0';
                    } else {
                        int is_neg = val < 0;
                        if (is_neg) val = -val;
                        
                        while (val > 0) {
                            *--num_ptr = '0' + (val % 10);
                            val /= 10;
                        }
                        
                        if (is_neg) *--num_ptr = '-';
                        else if (plus) *--num_ptr = '+';
                        else if (space) *--num_ptr = ' ';
                    }
                    
                    // Если precision задана, используем ее вместо width для padding
                    if (precision >= 0) {
                        zero_pad = true; // Для precision заполняем нулями
                        // Если width меньше, чем нужно для precision + знак, 
                        // то width = precision + (знак?1:0)
                        int len = num_buf + 31 - num_ptr;
                        int digits_needed = precision;
                        if (digits_needed > len) {
                            if (num_buf[31 - len] == '-' || num_buf[31 - len] == '+' || num_buf[31 - len] == ' ') {
                                digits_needed++; // Учитываем знак
                            }
                        }
                        if (width < digits_needed) {
                            width = digits_needed;
                        }
                    }
                    
                    // Дополнение пробелами/нулями
                    int len = num_buf + 31 - num_ptr;
                    if (width > len) {
                        int pad = width - len;
                        if (left_align) {
                            // Выравнивание влево: число потом пробелы
                            while (*num_ptr) *ptr++ = *num_ptr++;
                            while (pad-- > 0) *ptr++ = ' ';
                        } else {
                            // Выравнивание вправо
                            char pad_char = zero_pad ? '0' : ' ';
                            if (pad_char == '0') {
                                // Особый случай для отрицательных чисел с нулевым заполнением
                                if (num_buf[31 - len] == '-') {
                                    *ptr++ = '-';
                                    num_ptr++; // Пропускаем минус
                                    len--;
                                    while (pad-- > 0) *ptr++ = '0';
                                    while (*num_ptr) *ptr++ = *num_ptr++;
                                } else if (num_buf[31 - len] == '+' || num_buf[31 - len] == ' ') {
                                    *ptr++ = num_buf[31 - len];
                                    num_ptr++; // Пропускаем знак
                                    len--;
                                    while (pad-- > 0) *ptr++ = '0';
                                    while (*num_ptr) *ptr++ = *num_ptr++;
                                } else {
                                    while (pad-- > 0) *ptr++ = '0';
                                    while (*num_ptr) *ptr++ = *num_ptr++;
                                }
                            } else {
                                while (pad-- > 0) *ptr++ = ' ';
                                while (*num_ptr) *ptr++ = *num_ptr++;
                            }
                        }
                    } else {
                        while (*num_ptr) *ptr++ = *num_ptr++;
                    }
                    break;
                }
                
                case 'u': {
                    unsigned long long val;
                    if (is_long_long) {
                        val = va_arg(args, unsigned long long);
                    } else if (is_long) {
                        val = va_arg(args, unsigned long);
                    } else if (is_size_t) {
                        val = (unsigned long long)va_arg(args, size_t);
                    } else if (is_short) {
                        val = (unsigned short)va_arg(args, unsigned int);
                    } else if (is_char) {
                        val = (unsigned char)va_arg(args, unsigned int);
                    } else {
                        val = va_arg(args, unsigned int);
                    }
                    
                    char num_buf[32];
                    char* num_ptr = num_buf + 31;
                    *num_ptr = '\0';
                    
                    if (val == 0) {
                        *--num_ptr = '0';
                    } else {
                        while (val > 0) {
                            *--num_ptr = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    
                    // Precision handling
                    if (precision >= 0) {
                        zero_pad = true;
                        int len = num_buf + 31 - num_ptr;
                        if (width < precision) {
                            width = precision;
                        }
                    }
                    
                    // Дополнение
                    int len = num_buf + 31 - num_ptr;
                    if (width > len) {
                        int pad = width - len;
                        if (left_align) {
                            while (*num_ptr) *ptr++ = *num_ptr++;
                            while (pad-- > 0) *ptr++ = ' ';
                        } else {
                            char pad_char = zero_pad ? '0' : ' ';
                            while (pad-- > 0) *ptr++ = pad_char;
                            while (*num_ptr) *ptr++ = *num_ptr++;
                        }
                    } else {
                        while (*num_ptr) *ptr++ = *num_ptr++;
                    }
                    break;
                }
                
                case 'x':
                case 'X': {
                    unsigned long long val;
                    if (is_long_long) {
                        val = va_arg(args, unsigned long long);
                    } else if (is_long) {
                        val = va_arg(args, unsigned long);
                    } else if (is_size_t) {
                        val = (unsigned long long)va_arg(args, size_t);
                    } else {
                        val = va_arg(args, unsigned int);
                    }
                    
                    char hex_buf[20];
                    char* hex_ptr = hex_buf + 19;
                    *hex_ptr = '\0';
                    
                    const char* hex_digits = (*f == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                    
                    if (val == 0) {
                        *--hex_ptr = '0';
                    } else {
                        while (val > 0) {
                            *--hex_ptr = hex_digits[val & 0xF];
                            val >>= 4;
                        }
                    }
                    
                    // Дополнение
                    int len = hex_buf + 19 - hex_ptr;
                    if (width > len) {
                        int pad = width - len;
                        if (left_align) {
                            while (*hex_ptr) *ptr++ = *hex_ptr++;
                            while (pad-- > 0) *ptr++ = ' ';
                        } else {
                            char pad_char = zero_pad ? '0' : ' ';
                            while (pad-- > 0) *ptr++ = pad_char;
                            while (*hex_ptr) *ptr++ = *hex_ptr++;
                        }
                    } else {
                        while (*hex_ptr) *ptr++ = *hex_ptr++;
                    }
                    break;
                }
                
                case 's': {
                    char* str = va_arg(args, char*);
                    if (!str) str = "(null)";
                    
                    int max_len = strlen(str);
                    if (precision >= 0 && precision < max_len) {
                        max_len = precision;
                    }
                    
                    if (width > max_len) {
                        int pad = width - max_len;
                        if (left_align) {
                            // Строка потом пробелы
                            for (int i = 0; i < max_len; i++) *ptr++ = str[i];
                            while (pad-- > 0) *ptr++ = ' ';
                        } else {
                            // Пробелы потом строка
                            while (pad-- > 0) *ptr++ = ' ';
                            for (int i = 0; i < max_len; i++) *ptr++ = str[i];
                        }
                    } else {
                        for (int i = 0; i < max_len; i++) *ptr++ = str[i];
                    }
                    break;
                }
                
                case 'c': {
                    char c = (char)va_arg(args, int);
                    
                    if (width > 1) {
                        int pad = width - 1;
                        if (left_align) {
                            *ptr++ = c;
                            while (pad-- > 0) *ptr++ = ' ';
                        } else {
                            char pad_char = zero_pad ? '0' : ' ';
                            while (pad-- > 0) *ptr++ = pad_char;
                            *ptr++ = c;
                        }
                    } else {
                        *ptr++ = c;
                    }
                    break;
                }
                
                case 'p': {
                    void* val = va_arg(args, void*);
                    uintptr_t ival = (uintptr_t)val;
                    
                    char hex_buf[19];
                    char* hex_ptr = hex_buf + 18;
                    *hex_ptr = '\0';
                    
                    const char* hex_digits = "0123456789abcdef";
                    
                    if (ival == 0) {
                        *--hex_ptr = '0';
                    } else {
                        while (ival > 0) {
                            *--hex_ptr = hex_digits[ival & 0xF];
                            ival >>= 4;
                        }
                    }
                    
                    // Добавление нулей
                    int len = hex_buf + 18 - hex_ptr;
                    if (len < 16) {
                        while (len < 16) {
                            *--hex_ptr = '0';
                            len++;
                        }
                    }
                    
                    *--hex_ptr = 'x';
                    *--hex_ptr = '0';
                    
                    // Вывод с учётом ширины
                    len = hex_buf + 18 - hex_ptr;
                    if (width > len) {
                        int pad = width - len;
                        if (left_align) {
                            while (*hex_ptr) *ptr++ = *hex_ptr++;
                            while (pad-- > 0) *ptr++ = ' ';
                        } else {
                            while (pad-- > 0) *ptr++ = ' ';
                            while (*hex_ptr) *ptr++ = *hex_ptr++;
                        }
                    } else {
                        while (*hex_ptr) *ptr++ = *hex_ptr++;
                    }
                    break;
                }
                
                case '%': {
                    *ptr++ = '%';
                    break;
                }
                
                default: {
                    *ptr++ = '%';
                    *ptr++ = *f;
                    break;
                }
            }
            f++;
        } else {
            *ptr++ = *f++;
        }
    }
    
    *ptr = '\0';
    va_end(args);
    
    term_puts(term, buffer);
}

void term_clear_prompt(term_t* term) {
    if (!term->prompt_enabled) return;
    
    term->input_pos = 0;
    term->input_cursor = 0;
    term->input_buffer[0] = '\0';
    term->needs_redraw = true;
    
    // Перерисовываем экран
    refresh_screen(term);
}