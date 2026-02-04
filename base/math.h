#ifndef MATH_H
#define MATH_H

#include <stdint.h>

// Абсолютное значение для целых чисел
static inline int abs(int n) {
    return (n < 0) ? -n : n;
}

static inline long labs(long n) {
    return (n < 0) ? -n : n;
}

static inline long long llabs(long long n) {
    return (n < 0) ? -n : n;
}

// Абсолютное значение для чисел с плавающей точкой
static inline float fabsf(float x) {
    union {
        float f;
        uint32_t i;
    } u = {x};
    u.i &= 0x7fffffff;
    return u.f;
}

static inline double fabs(double x) {
    union {
        double d;
        uint64_t i;
    } u = {x};
    u.i &= 0x7fffffffffffffffULL;
    return u.d;
}

// Максимум и минимум
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// Ограничение значения
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

// Знак числа
static inline int sign(int x) {
    return (x > 0) - (x < 0);
}

static inline int signf(float x) {
    return (x > 0.0f) - (x < 0.0f);
}

#endif // MATH_H
