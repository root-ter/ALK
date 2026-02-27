// base/term/tio.c
#include "tio.h"
#include "term.h"
#include <stdarg.h>

static term_t* g_term = NULL;
static volatile int g_lock = 0;

static inline void _lock(void) {
    while (__sync_lock_test_and_set(&g_lock, 1)) {
        asm volatile("pause");
    }
}

static inline void _unlock(void) {
    __sync_synchronize();
    __sync_lock_release(&g_lock);
}

void tio_init(void* term_ptr) {
    _lock();
    g_term = (term_t*)term_ptr;
    _unlock();
}

static inline int _term_alive(void) {
    return g_term != NULL;
}

// ==================== ВЫВОД ====================

void tio_printf(const char* fmt, ...) {
    if (!_term_alive()) return;
    
    _lock();
    
    va_list args;
    va_start(args, fmt);
    term_printf(g_term, fmt, args);
    va_end(args);
    
    _unlock();
}

void tio_puts(const char* s) {
    if (!_term_alive() || !s) return;
    
    _lock();
    term_puts(g_term, s);
    _unlock();
}

void tio_putc(char c) {
    if (!_term_alive()) return;
    
    _lock();
    term_putc(g_term, c);
    _unlock();
}

// ==================== УПРАВЛЕНИЕ ====================

void tio_clear(void) {
    if (!_term_alive()) return;
    
    _lock();
    term_clear(g_term);
    _unlock();
}

void tio_set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (!_term_alive() || !g_term->fb) return;
    
    _lock();
    g_term->fg_color = (color_t){r, g, b};
    _unlock();
}

void tio_enable_prompt(void) {
    if (!_term_alive()) return;
    
    _lock();
    term_enable_prompt(g_term);
    _unlock();
}

void tio_disable_prompt(void) {
    if (!_term_alive()) return;
    
    _lock();
    term_disable_prompt(g_term);
    _unlock();
}

void tio_set_prompt(const char* text) {
    if (!_term_alive() || !text) return;
    
    _lock();
    term_set_prompt_text(g_term, text);
    _unlock();
}

// ==================== БЛОКИРОВКИ ====================

void tio_lock(void) {
    _lock();
}

void tio_unlock(void) {
    _unlock();
}

int tio_initialized(void) {
    return g_term != NULL;
}