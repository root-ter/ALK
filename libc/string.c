#include <stddef.h>
#include "string.h"
#include <stdint.h>
#include <stdarg.h>

/* =================== MEM =================== */
void *memcpy(void *dst_, const void *src_, size_t n)
{
    unsigned char *dst = (unsigned char *)dst_;
    const unsigned char *src = (const unsigned char *)src_;
    void *ret = dst_;

    if (n == 0 || dst == src)
        return ret;

#if defined(__x86_64__) || defined(__i386__)
    /* На x86/x86_64 rep movsb очень быстрый */
    asm volatile(
        "rep movsb"
        : "+D"(dst), "+S"(src), "+c"(n)
        :
        : "memory");
    return ret;
#else
    const size_t W = sizeof(unsigned long);
    uintptr_t dst_addr = (uintptr_t)dst;
    uintptr_t src_addr = (uintptr_t)src;

    /* Выравниваем dst по границе слова побайтно */
    while (n > 0 && (dst_addr & (W - 1)))
    {
        *dst++ = *src++;
        --n;
        dst_addr = (uintptr_t)dst;
        src_addr = (uintptr_t)src;
    }

    /* Если выравнивание src равно выравниванию dst, можно копировать словами */
    if ((src_addr & (W - 1)) == (dst_addr & (W - 1)))
    {
        unsigned long *dw = (unsigned long *)dst;
        const unsigned long *sw = (const unsigned long *)src;
        size_t words = n / W;

        /* Разворачивание цикла по 4 слова */
        while (words >= 4)
        {
            dw[0] = sw[0];
            dw[1] = sw[1];
            dw[2] = sw[2];
            dw[3] = sw[3];
            dw += 4;
            sw += 4;
            words -= 4;
        }
        while (words--)
        {
            *dw++ = *sw++;
        }

        /* Обновляем указатели и оставшиеся байты */
        dst = (unsigned char *)dw;
        src = (const unsigned char *)sw;
        n = n & (W - 1);
    }
    else
    {
        /* Если выравнивания не совпадают - оставляем всё побайтно.
           Это безопасно на архитектурах с требованием выравнивания. */
    }

    /* Хвостовые байты */
    while (n--)
    {
        *dst++ = *src++;
    }

    return ret;
#endif
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char val = (unsigned char)c;
    void *ret = s;

    const size_t W = sizeof(unsigned long);
    uintptr_t addr = (uintptr_t)p;

    /* Выравниваем по границе слова побайтно */
    while (n > 0 && (addr & (W - 1)))
    {
        *p++ = val;
        --n;
        addr = (uintptr_t)p;
    }

    /* Заполняем целыми словами */
    if (n >= W)
    {
        unsigned long word = val;
        word |= word << 8;
        word |= word << 16;
#ifdef __LP64__
        word |= word << 32;
#endif
        unsigned long *pw = (unsigned long *)p;
        while (n >= W)
        {
            *pw++ = word;
            n -= W;
        }
        p = (unsigned char *)pw;
    }

    /* Оставшиеся байты */
    while (n--)
        *p++ = val;

    return ret;
}

int memcmp(const void *ptr1, const void *ptr2, size_t num)
{
    const unsigned char *a = (const unsigned char *)ptr1;
    const unsigned char *b = (const unsigned char *)ptr2;

    for (size_t i = 0; i < num; i++)
    {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

void *memmove(void *dst0, const void *src0, size_t n)
{
    if (n == 0 || dst0 == src0)
        return dst0;

    unsigned char *dst = (unsigned char *)dst0;
    const unsigned char *src = (const unsigned char *)src0;

    if (dst < src) /* копируем вперед */
    {
        size_t word = sizeof(uintptr_t);
        while (n && ((uintptr_t)dst & (word - 1)))
        {
            *dst++ = *src++;
            --n;
        }

        uintptr_t *dw = (uintptr_t *)dst;
        const uintptr_t *sw = (const uintptr_t *)src;
        while (n >= word)
        {
            *dw++ = *sw++;
            n -= word;
        }

        dst = (unsigned char *)dw;
        src = (const unsigned char *)sw;
        while (n--)
            *dst++ = *src++;
    }
    else /* dst > src - копируем назад */
    {
        dst += n;
        src += n;

        size_t word = sizeof(uintptr_t);
        while (n && ((uintptr_t)dst & (word - 1)))
        {
            *--dst = *--src;
            --n;
        }

        uintptr_t *dw = (uintptr_t *)dst;
        const uintptr_t *sw = (const uintptr_t *)src;
        while (n >= word)
        {
            *--dw = *--sw;
            n -= word;
        }

        dst = (unsigned char *)dw;
        src = (const unsigned char *)sw;
        while (n--)
            *--dst = *--src;
    }

    return dst0;
}

/* =================== STR =================== */
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a == *b && *a != '\0')
    {
        a++;
        b++;
    }
    return *(unsigned char *)a - *(unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    while (*s)
    {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    if ((char)c == '\0')
        return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s)
    {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if ((char)c == '\0')
        return (char *)s;
    return (char *)last;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (*d)
        ++d; /* найдём конец dest */
    size_t i = 0;
    while (i < n && src[i] != '\0')
    {
        d[i] = src[i];
        ++i;
    }
    d[i] = '\0';
    return dest;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *token;

    if (str)
        *saveptr = str;
    if (*saveptr == NULL)
        return NULL;

    // Пропускаем ведущие символы-разделители
    char *start = *saveptr;
    while (*start && strchr(delim, *start))
        start++;
    if (*start == '\0')
    {
        *saveptr = NULL;
        return NULL;
    }

    // Найти конец токена
    token = start;
    char *p = start;
    while (*p && !strchr(delim, *p))
        p++;

    if (*p)
    {
        *p = '\0';
        *saveptr = p + 1;
    }
    else
    {
        *saveptr = NULL;
    }

    return token;
}

int nameeq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        char ca = a[i], cb = b[i];
        if (!ca && !cb)
            return 1;
        if (ca != cb)
            return 0;
    }
    return 1;
}

static char* itoa(int value, char* str, int base) {
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    int tmp_value;
    
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }
    
    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[tmp_value - value * base];
    } while (value);
    
    *ptr-- = '\0';
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
    return str;
}

static char* utoa(unsigned int value, char* str, int base) {
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    unsigned int tmp_value;
    
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }
    
    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[tmp_value - value * base];
    } while (value);
    
    *ptr-- = '\0';
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
    return str;
}

// Основная функция vsprintf
int vsprintf(char* str, const char* format, va_list args) {
    char* ptr = str;
    const char* fmt = format;
    char buffer[32];
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': {
                    char* s = va_arg(args, char*);
                    while (*s) {
                        *ptr++ = *s++;
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    *ptr++ = c;
                    break;
                }
                case 'd':
                case 'i': {
                    int n = va_arg(args, int);
                    itoa(n, buffer, 10);
                    char* s = buffer;
                    while (*s) {
                        *ptr++ = *s++;
                    }
                    break;
                }
                case 'u': {
                    unsigned int n = va_arg(args, unsigned int);
                    utoa(n, buffer, 10);
                    char* s = buffer;
                    while (*s) {
                        *ptr++ = *s++;
                    }
                    break;
                }
                case 'x': {
                    unsigned int n = va_arg(args, unsigned int);
                    utoa(n, buffer, 16);
                    char* s = buffer;
                    while (*s) {
                        *ptr++ = *s++;
                    }
                    break;
                }
                case 'p': {
                    uintptr_t n = va_arg(args, uintptr_t);
                    utoa(n, buffer, 16);
                    *ptr++ = '0';
                    *ptr++ = 'x';
                    char* s = buffer;
                    while (*s) {
                        *ptr++ = *s++;
                    }
                    break;
                }
                case 'l': {
                    fmt++;
                    if (*fmt == 'l') {
                        fmt++;
                        if (*fmt == 'u') {
                            unsigned long long n = va_arg(args, unsigned long long);
                            // Простая реализация для 64-bit
                            char* s = buffer;
                            // Конвертируем в hex (проще)
                            utoa((unsigned int)(n >> 32), buffer, 16);
                            s = buffer;
                            while (*s) *ptr++ = *s++;
                            utoa((unsigned int)(n & 0xFFFFFFFF), buffer, 16);
                            s = buffer;
                            while (*s) *ptr++ = *s++;
                        }
                    }
                    break;
                }
                case '%': {
                    *ptr++ = '%';
                    break;
                }
            }
            fmt++;
        } else {
            *ptr++ = *fmt++;
        }
    }
    
    *ptr = '\0';
    return ptr - str;
}

// vsnprintf с ограничением длины
int vsnprintf(char* str, size_t size, const char* format, va_list args) {
    if (size == 0) return 0;
    
    char buffer[1024];  // Временный буфер
    int len = vsprintf(buffer, format, args);
    
    if (len >= (int)size) {
        memcpy(str, buffer, size - 1);
        str[size - 1] = '\0';
        return size - 1;
    } else {
        memcpy(str, buffer, len + 1);
        return len;
    }
}

// snprintf
int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

// sprintf (простая обертка)
int sprintf(char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsprintf(str, format, args);
    va_end(args);
    return result;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) {
        return NULL;
    }
    
    // Если needle пустая строка, возвращаем haystack
    if (*needle == '\0') {
        return (char *)haystack;
    }
    
    // Быстрая проверка на случай, если needle длиннее haystack
    size_t needle_len = strlen(needle);
    size_t haystack_len = strlen(haystack);
    
    if (needle_len > haystack_len) {
        return NULL;
    }
    
    // Оптимизированный алгоритм с предварительной проверкой первого символа
    char first = needle[0];
    
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        // Быстрая проверка первого символа
        if (haystack[i] != first) {
            continue;
        }
        
        // Проверяем остальные символы
        size_t j;
        for (j = 1; j < needle_len; j++) {
            if (haystack[i + j] != needle[j]) {
                break;
            }
        }
        
        // Если все символы совпали, возвращаем указатель
        if (j == needle_len) {
            return (char *)(haystack + i);
        }
    }
    
    return NULL;
}

int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    if (!str) return 0;
    
    // Пропускаем пробелы
    while (*str == ' ' || *str == '\t') str++;
    
    // Обработка знака
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Конвертация цифр
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

// Конвертация строки в длинное целое
long atol(const char* str) {
    long result = 0;
    int sign = 1;
    
    if (!str) return 0;
    
    // Пропускаем пробелы
    while (*str == ' ' || *str == '\t') str++;
    
    // Обработка знака
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Конвертация цифр
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}