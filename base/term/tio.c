// base/term/tio.c
#include "tio.h"
#include "term.h"
#include <stdarg.h>
#include "../../libc/string.h"

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
            int zero_pad = 0;
            int left_align = 0;
            int alternate = 0;
            int space = 0;
            int plus = 0;
            
            // Обработка флагов
            while (1) {
                if (*f == '-') {
                    left_align = 1;
                    f++;
                } else if (*f == '0') {
                    zero_pad = 1;
                    f++;
                } else if (*f == '#') {
                    alternate = 1;
                    f++;
                } else if (*f == ' ') {
                    space = 1;
                    f++;
                } else if (*f == '+') {
                    plus = 1;
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
            int is_long = 0;
            int is_long_long = 0;
            int is_size_t = 0;
            int is_short = 0;
            int is_char = 0;
            
            if (*f == 'h') {
                f++;
                if (*f == 'h') {
                    is_char = 1;
                    f++;
                } else {
                    is_short = 1;
                }
            } else if (*f == 'l') {
                f++;
                if (*f == 'l') {
                    is_long_long = 1;
                    f++;
                } else {
                    is_long = 1;
                }
            } else if (*f == 'z') {
                is_size_t = 1;
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
                    
                    // Precision handling
                    if (precision >= 0) {
                        zero_pad = 1;
                        int len = num_buf + 31 - num_ptr;
                        int digits_needed = precision;
                        if (digits_needed > len) {
                            if (num_buf[31 - len] == '-' || num_buf[31 - len] == '+' || num_buf[31 - len] == ' ') {
                                digits_needed++;
                            }
                        }
                        if (width < digits_needed) {
                            width = digits_needed;
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
                            if (pad_char == '0') {
                                if (num_buf[31 - len] == '-') {
                                    *ptr++ = '-';
                                    num_ptr++;
                                    len--;
                                    while (pad-- > 0) *ptr++ = '0';
                                    while (*num_ptr) *ptr++ = *num_ptr++;
                                } else if (num_buf[31 - len] == '+' || num_buf[31 - len] == ' ') {
                                    *ptr++ = num_buf[31 - len];
                                    num_ptr++;
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
                        zero_pad = 1;
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
                            for (int i = 0; i < max_len; i++) *ptr++ = str[i];
                            while (pad-- > 0) *ptr++ = ' ';
                        } else {
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
    
    term_puts(g_term, buffer);
    
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