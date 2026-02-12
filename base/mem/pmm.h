// mem/pmm.h
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Размер страницы - 4KB (стандарт для x86_64)
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

// Максимальный размер битмапа - 32MB = 256GB памяти
// Если у кого-то больше 256GB ОЗУ - ну хрен знает, пусть свой PMM пишут
#define PMM_BITMAP_MAX_SIZE (32 * 1024 * 1024)

// Структура физического менеджера памяти
typedef struct {
    uint8_t* bitmap;        // Указатель на битовый массив
    uint64_t base_addr;     // Начальный физический адрес
    uint32_t total_pages;   // Всего страниц в управлении
    uint32_t used_pages;    // Занятых страниц
    uint64_t bitmap_size;   // Размер битмапа в байтах
} pmm_t;

// Инициализация PMM с использованием карты памяти из Multiboot2
// Принимает: указатель на структуру PMM, адрес структуры Multiboot2
// Возвращает: ничего, но если памяти нет - будет плохо
void pmm_init(pmm_t* pmm, uint64_t mb2_addr);

// Выделить ОДНУ физическую страницу (4096 байт)
// Принимает: указатель на структуру PMM
// Возвращает: физический адрес страницы или NULL если памяти нет
void* pmm_alloc_page(pmm_t* pmm);

// Освободить ОДНУ физическую страницу
// Принимает: указатель на структуру PMM, физический адрес страницы
// Возвращает: ничего, но если адрес кривой - молча игнорит
void pmm_free_page(pmm_t* pmm, void* addr);

// Получить количество свободных страниц
static inline uint32_t pmm_get_free_pages(pmm_t* pmm) {
    return pmm ? (pmm->total_pages - pmm->used_pages) : 0;
}

// Получить количество занятых страниц
static inline uint32_t pmm_get_used_pages(pmm_t* pmm) {
    return pmm ? pmm->used_pages : 0;
}
  
#endif // PMM_H
