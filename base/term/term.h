#ifndef TERM_H
#define TERM_H

#include "../../drv/fb/fb.h"

// Терминал с историей и промптом
typedef struct {
    framebuffer_t* fb;
    
    // Геометрия
    uint32_t x, y;                    // Позиция в пикселях
    uint32_t cols, rows;              // Размер в символах
    uint32_t char_width, char_height; // Размер символа с интервалами
    
    // Состояние отображения
    uint32_t cursor_x, cursor_y;      // Текущая позиция курсора (в символах)
    uint32_t scroll_offset;           // Смещение для прокрутки
    uint32_t total_lines;             // Всего строк в истории
    
    // Буфер истории (кольцевой)
    char* history_buffer;             // Буфер всех строк
    uint32_t history_size;            // Максимальное количество строк
    uint32_t history_start;           // Индекс начала
    uint32_t history_end;             // Индекс конца
    
    // Буфер экрана (для отображения)
    char* screen_buffer;              // Буфер видимых строк
    uint32_t screen_size;             // rows * (cols + 1)
    
    // Цвета
    color_t fg_color;
    color_t bg_color;
    
    // Промпт и ввод
    bool prompt_enabled;
    char* input_buffer;               // Буфер ввода
    uint32_t input_pos;               // Текущая позиция в буфере
    uint32_t input_capacity;          // Ёмкость буфера
    uint32_t input_cursor;            // Позиция курсора в буфере
    char prompt_text[32];             // Текст промпта
    
    // Флаги состояния
    bool needs_redraw;                // Нужна перерисовка

    volatile int lock;
} term_t;

// Инициализация
term_t* term_init(framebuffer_t* fb, uint32_t x, uint32_t y, 
                  uint32_t cols, uint32_t rows);

// Основные функции
void term_clear(term_t* term);
void term_putc(term_t* term, char c);
void term_puts(term_t* term, const char* str);
void term_printf(term_t* term, const char* fmt, ...);

// Прокрутка
void term_scroll_up(term_t* term, uint32_t lines);
void term_scroll_down(term_t* term, uint32_t lines);

// Управление промптом
void term_enable_prompt(term_t* term);
void term_disable_prompt(term_t* term);
bool term_is_prompt_enabled(term_t* term);
void term_set_prompt_text(term_t* term, const char* text);
void term_clear_input(term_t* term);

// Обработка ввода с клавиатуры
bool term_handle_input(term_t* term, char input_char, char** out_line);

// Вспомогательные функции
void term_set_color(term_t* term, color_t fg, color_t bg);
void term_set_cursor(term_t* term, uint32_t x, uint32_t y);
void term_get_size(term_t* term, uint32_t* cols, uint32_t* rows);
void term_get_cursor(term_t* term, uint32_t* x, uint32_t* y);

void term_clear_prompt(term_t* term);

#endif