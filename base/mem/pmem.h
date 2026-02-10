#ifndef PMEM_H
#define PMEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SIZE 4096
#define BITS_PER_BYTE 8

// Флаги для выделения памяти
#define PMEM_FLAG_NORMAL  0x00
#define PMEM_FLAG_DMA     0x01  // Для DMA (выровнено по 4KB)
#define PMEM_FLAG_UNCACHEABLE 0x02  // Не кэшируемая память
#define PMEM_FLAG_RESERVED 0x04  // Зарезервированная область

// Структура региона памяти
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t flags;
} pmem_region_t;

// Основная структура аллокатора
typedef struct {
    uint8_t* bitmap;           // Bitmap свободных страниц
    uint64_t bitmap_size;      // Размер bitmap в байтах
    uint64_t total_pages;      // Всего страниц в системе
    uint64_t free_pages;       // Свободных страниц
    uint64_t used_pages;       // Использованных страниц
    uint64_t reserved_pages;   // Зарезервированных страниц
    uint64_t last_alloc_idx;   // Для оптимизации поиска (next-fit)
    
    pmem_region_t regions[32]; // Регионы памяти из Memory Map
    uint32_t region_count;
    
    // Статистика
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t failed_allocs;
} pmem_allocator_t;

// Инициализация аллокатора
int pmem_init(uint64_t memory_size);

// Основные функции
void* pmem_alloc(size_t pages, uint32_t flags);
void pmem_free(void* ptr, size_t pages); // Освобождение страниц

// Функции получения информации - РАЗНЫЕ ИМЕНА!
uint64_t pmem_get_total(void);    // вместо pmem_total
uint64_t pmem_get_free(void);     // вместо pmem_free (конфликт!)
uint64_t pmem_get_used(void);     // вместо pmem_used
void pmem_stats(void);
void pmem_dump_map(void);

// Выделение с выравниванием
void* pmem_alloc_aligned(size_t pages, uint32_t alignment, uint32_t flags);

// Вспомогательные функции
static inline uint64_t va_to_pa(void* va) { return (uint64_t)va; }
static inline void* pa_to_va(uint64_t pa) { return (void*)pa; }
static inline uint64_t bytes_to_pages(size_t bytes) { 
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE; 
}

#endif // PMEM_H
