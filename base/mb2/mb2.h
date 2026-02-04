#ifndef MB2_H
#define MB2_H

#include <stdint.h>
#include <stddef.h>

#define MB2_MEMORY_AVAILABLE 1  // ДОБАВИЛИ!
#define MB2_MEMORY_RESERVED 2
#define MB2_MEMORY_ACPI_RECLAIMABLE 3
#define MB2_MEMORY_NVS 4
#define MB2_MEMORY_BADRAM 5

typedef struct __attribute__((packed))
{
    uint32_t type; /* тип тега (например, 8 = framebuffer) */
    uint32_t size; /* общий размер тега в байтах (включая эти 8 байт) */
} mb2_tag_t;

typedef struct
{
    uint64_t addr;   /* физический адрес начала фреймбуфера */
    uint32_t pitch;  /* количество байт в одной строке (0, если неизвестно) */
    uint32_t width;  /* ширина экрана в пикселях */
    uint32_t height; /* высота экрана в пикселях */
    uint8_t bpp;     /* количество бит на пиксель (0, если неизвестно) */
    uint8_t fb_type; /* тип фреймбуфера (например, RGB = 1, текстовый = 2 и т.п.) */
} framebuffer_info_t;

// Memory Map Entry (Memory Area)
typedef struct __attribute__((packed)) {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} mb2_mmap_entry_t;

// Memory Map Tag
typedef struct __attribute__((packed)) {
    uint32_t type;          // = 6
    uint32_t size;          // Размер всего тега
    uint32_t entry_size;    // Размер одной записи (обычно 24)
    uint32_t entry_version; // Версия (0)
    // Далее массив записей mb2_mmap_entry_t
} mb2_mmap_tag_t;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    const char* description;
} mem_region_t;

void mb2_parse(uint64_t mb2_addr);
framebuffer_info_t *get_framebuffer_info(void);
uint64_t get_rsdp_address(void);
uint64_t get_total_memory(void);
uint64_t mb2_get_usable_memory(void);
int mb2_get_memory_regions(mem_region_t *regions, int max_count);
void mb2_dump_memory_map(void);

#endif /* MB2_H */
