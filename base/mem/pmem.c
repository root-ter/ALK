#include "pmem.h"
#include "../mb2/mb2.h"
#include "../../libc/string.h"
#include "../term/term.h"
#include "../rsod/rsod.h"
#include "mem.h"
#include <stddef.h>

// Глобальный аллокатор
static pmem_allocator_t g_pmem;
extern term_t* term;

// Магические числа для проверки целостности
#define PMEM_MAGIC_ALLOC 0xDEADBEEF
#define PMEM_MAGIC_FREE  0xCAFEBABE

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

// Установка бита в bitmap
static inline void bitmap_set(uint8_t* bitmap, uint64_t bit) {
    bitmap[bit / BITS_PER_BYTE] |= (1 << (bit % BITS_PER_BYTE));
}

// Сброс бита в bitmap
static inline void bitmap_clear(uint8_t* bitmap, uint64_t bit) {
    bitmap[bit / BITS_PER_BYTE] &= ~(1 << (bit % BITS_PER_BYTE));
}

// Проверка бита в bitmap
static inline bool bitmap_test(const uint8_t* bitmap, uint64_t bit) {
    return (bitmap[bit / BITS_PER_BYTE] & (1 << (bit % BITS_PER_BYTE))) != 0;
}

// Поиск последовательных свободных страниц (first-fit)
static int64_t find_free_pages(size_t pages, uint64_t start_idx) {
    if (pages == 0 || pages > g_pmem.total_pages) return -1;
    
    uint64_t consecutive = 0;
    uint64_t start = start_idx;
    
    // Оптимизация: начинаем с last_alloc_idx для next-fit
    if (start_idx == 0 && g_pmem.last_alloc_idx != 0) {
        start = g_pmem.last_alloc_idx;
    }
    
    for (uint64_t i = start; i < g_pmem.total_pages; i++) {
        if (!bitmap_test(g_pmem.bitmap, i)) {
            consecutive++;
            if (consecutive == pages) {
                g_pmem.last_alloc_idx = (i + 1) % g_pmem.total_pages;
                return i - pages + 1; // Возвращаем первую страницу блока
            }
        } else {
            consecutive = 0;
        }
        
        // Если дошли до конца, начинаем сначала
        if (i == g_pmem.total_pages - 1 && start > 0) {
            i = 0;
            start = 0; // Чтобы не зациклиться
        }
    }
    
    // Попробуем с начала
    if (start_idx != 0) {
        return find_free_pages(pages, 0);
    }
    
    return -1;
}

// Проверка, что адрес выровнен
static inline bool is_aligned(uint64_t addr, uint64_t alignment) {
    return (addr & (alignment - 1)) == 0;
}

// ==================== ИНИЦИАЛИЗАЦИЯ ====================

int pmem_init(uint64_t memory_size) {
    term_printf(term, "[PMEM] Initializing physical memory allocator...\n");
    
    // Очищаем структуру
    memset(&g_pmem, 0, sizeof(pmem_allocator_t));
    
    // Получаем информацию о памяти из Multiboot2
    mem_region_t regions[32];
    int region_count = mb2_get_memory_regions(regions, 32);
    
    if (region_count == 0) {
        term_printf(term, "[PMEM] No memory regions found, falling back to 64MB\n");
        g_pmem.total_pages = (64 * 1024 * 1024) / PAGE_SIZE;
    } else {
        // Вычисляем общий объем доступной памяти
        uint64_t total_memory = 0;
        for (int i = 0; i < region_count; i++) {
            if (regions[i].type == MB2_MEMORY_AVAILABLE) {
                total_memory += regions[i].length;
                
                // Сохраняем регион
                g_pmem.regions[g_pmem.region_count].base = regions[i].base;
                g_pmem.regions[g_pmem.region_count].length = regions[i].length;
                g_pmem.regions[g_pmem.region_count].type = regions[i].type;
                g_pmem.regions[g_pmem.region_count].flags = PMEM_FLAG_NORMAL;
                g_pmem.region_count++;
                
                term_printf(term, "[PMEM] Region %d: 0x%llx - 0x%llx (%llu MB)\n",
                           i, regions[i].base, regions[i].base + regions[i].length,
                           regions[i].length / (1024 * 1024));
            }
        }
        
        if (total_memory == 0) {
            rsod("NO_AVAILABLE_MEMORY", "PMEM");
        }
        
        g_pmem.total_pages = total_memory / PAGE_SIZE;
    }
    
    term_printf(term, "[PMEM] Total memory: %llu pages (%llu MB)\n",
               g_pmem.total_pages, 
               (g_pmem.total_pages * PAGE_SIZE) / (1024 * 1024));
    
    // Выделяем bitmap из kernel heap (пока нет физического аллокатора)
    g_pmem.bitmap_size = (g_pmem.total_pages + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
    term_printf(term, "[PMEM] Bitmap size: %llu bytes\n", g_pmem.bitmap_size);
    
    g_pmem.bitmap = (uint8_t*)malloc(g_pmem.bitmap_size);
    if (!g_pmem.bitmap) {
        rsod("PMEM_BITMAP_ALLOC_FAILED", "PMEM");
    }
    
    // Инициализируем bitmap (вся память занята)
    memset(g_pmem.bitmap, 0xFF, g_pmem.bitmap_size);
    
    // Помечаем доступные регионы как свободные
    g_pmem.free_pages = 0;
    for (uint32_t i = 0; i < g_pmem.region_count; i++) {
        if (g_pmem.regions[i].type == MB2_MEMORY_AVAILABLE) {
            uint64_t start_page = g_pmem.regions[i].base / PAGE_SIZE;
            uint64_t end_page = (g_pmem.regions[i].base + g_pmem.regions[i].length) / PAGE_SIZE;
            
            for (uint64_t page = start_page; page < end_page; page++) {
                bitmap_clear(g_pmem.bitmap, page);
                g_pmem.free_pages++;
            }
            
            term_printf(term, "[PMEM] Marked region %d as free: pages %llu-%llu\n",
                       i, start_page, end_page - 1);
        }
    }
    
    // Зарезервируем первые 16MB для ядра (обычно там находятся ядро, bitmap и т.д.)
    uint64_t kernel_pages = (16 * 1024 * 1024) / PAGE_SIZE;
    for (uint64_t page = 0; page < kernel_pages && page < g_pmem.total_pages; page++) {
        if (!bitmap_test(g_pmem.bitmap, page)) {
            bitmap_set(g_pmem.bitmap, page);
            g_pmem.free_pages--;
            g_pmem.reserved_pages++;
        }
    }
    
    g_pmem.used_pages = 0;
    g_pmem.last_alloc_idx = 0;
    
    term_printf(term, "[PMEM] Initialized: free=%llu, reserved=%llu, total=%llu\n",
               g_pmem.free_pages, g_pmem.reserved_pages, g_pmem.total_pages);
    
    return 0;
}

// ==================== ОСНОВНЫЕ ФУНКЦИИ ====================

void* pmem_alloc(size_t pages, uint32_t flags) {
    if (pages == 0 || pages > g_pmem.free_pages) {
        g_pmem.failed_allocs++;
        return NULL;
    }
    
    // Находим свободные страницы
    int64_t first_page = find_free_pages(pages, g_pmem.last_alloc_idx);
    if (first_page < 0) {
        term_printf(term, "[PMEM] Failed to allocate %zu pages\n", pages);
        g_pmem.failed_allocs++;
        return NULL;
    }
    
    // Помечаем страницы как занятые
    for (size_t i = 0; i < pages; i++) {
        bitmap_set(g_pmem.bitmap, first_page + i);
    }
    
    g_pmem.free_pages -= pages;
    g_pmem.used_pages += pages;
    g_pmem.total_allocs++;
    
    // Вычисляем физический адрес
    uint64_t phys_addr = first_page * PAGE_SIZE;
    
    // Для DMA проверяем выравнивание
    if ((flags & PMEM_FLAG_DMA) && !is_aligned(phys_addr, PAGE_SIZE)) {
        // Освобождаем и ищем заново
        pmem_free((void*)phys_addr, pages);
        return pmem_alloc_aligned(pages, PAGE_SIZE, flags);
    }
    
    // Преобразуем в виртуальный адрес (прямое отображение 1:1)
    void* virt_addr = pa_to_va(phys_addr);
    
    term_printf(term, "[PMEM] Allocated %zu pages at 0x%llx (0x%p)\n",
               pages, phys_addr, virt_addr);
    
    return virt_addr;
}

void pmem_free(void* ptr, size_t pages) {
    if (!ptr || pages == 0) return;
    
    uint64_t phys_addr = va_to_pa(ptr);
    uint64_t first_page = phys_addr / PAGE_SIZE;
    
    // Проверка границ
    if (first_page + pages > g_pmem.total_pages) {
        term_printf(term, "[PMEM] ERROR: Free out of bounds: 0x%llx, %zu pages\n",
                   phys_addr, pages);
        return;
    }
    
    // Проверяем, что страницы действительно были выделены
    bool all_allocated = true;
    for (size_t i = 0; i < pages; i++) {
        if (!bitmap_test(g_pmem.bitmap, first_page + i)) {
            all_allocated = false;
            break;
        }
    }
    
    if (!all_allocated) {
        term_printf(term, "[PMEM] WARNING: Freeing unallocated pages: 0x%llx\n",
                   phys_addr);
        return;
    }
    
    // Освобождаем страницы
    for (size_t i = 0; i < pages; i++) {
        bitmap_clear(g_pmem.bitmap, first_page + i);
    }
    
    g_pmem.free_pages += pages;
    g_pmem.used_pages -= pages;
    g_pmem.total_frees++;
    
    term_printf(term, "[PMEM] Freed %zu pages at 0x%llx\n", pages, phys_addr);
}

void* pmem_alloc_aligned(size_t pages, uint32_t alignment, uint32_t flags) {
    if (pages == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return NULL; // alignment должно быть степенью двойки
    }
    
    // Ищем с учетом выравнивания
    for (uint64_t i = 0; i < g_pmem.total_pages; i++) {
        uint64_t phys_addr = i * PAGE_SIZE;
        
        // Проверяем выравнивание
        if (!is_aligned(phys_addr, alignment)) {
            continue;
        }
        
        // Проверяем, что все страницы свободны
        bool all_free = true;
        for (size_t j = 0; j < pages; j++) {
            if (i + j >= g_pmem.total_pages || 
                bitmap_test(g_pmem.bitmap, i + j)) {
                all_free = false;
                break;
            }
        }
        
        if (all_free) {
            // Выделяем
            for (size_t j = 0; j < pages; j++) {
                bitmap_set(g_pmem.bitmap, i + j);
            }
            
            g_pmem.free_pages -= pages;
            g_pmem.used_pages += pages;
            g_pmem.total_allocs++;
            
            return pa_to_va(phys_addr);
        }
    }
    
    g_pmem.failed_allocs++;
    return NULL;
}

// ==================== ИНФОРМАЦИОННЫЕ ФУНКЦИИ ====================

uint64_t pmem_total(void) {
    return g_pmem.total_pages * PAGE_SIZE;
}

uint64_t pmem_used(void) {
    return g_pmem.used_pages * PAGE_SIZE;
}

uint64_t pmem_free_bytes(void) {
    return g_pmem.free_pages * PAGE_SIZE;
}

void pmem_stats(void) {
    if (!term) return;
    
    term_printf(term, "\n=== Physical Memory Statistics ===\n");
    term_printf(term, "Total:      %llu MB\n", pmem_total() / (1024 * 1024));
    term_printf(term, "Free:       %llu MB\n", pmem_free_bytes() / (1024 * 1024));
    term_printf(term, "Used:       %llu MB\n", pmem_used() / (1024 * 1024));
    term_printf(term, "Reserved:   %llu MB\n", 
                g_pmem.reserved_pages * PAGE_SIZE / (1024 * 1024));
    term_printf(term, "Allocations: %llu\n", g_pmem.total_allocs);
    term_printf(term, "Frees:       %llu\n", g_pmem.total_frees);
    term_printf(term, "Failed:      %llu\n", g_pmem.failed_allocs);
    
    // Распределение по регионам
    term_printf(term, "\nMemory Regions:\n");
    for (uint32_t i = 0; i < g_pmem.region_count; i++) {
        const char* type_str = "Unknown";
        switch (g_pmem.regions[i].type) {
            case MB2_MEMORY_AVAILABLE: type_str = "Available"; break;
            case MB2_MEMORY_RESERVED: type_str = "Reserved"; break;
            case MB2_MEMORY_ACPI_RECLAIMABLE: type_str = "ACPI"; break;
            case MB2_MEMORY_NVS: type_str = "NVS"; break;
            case MB2_MEMORY_BADRAM: type_str = "BadRAM"; break;
        }
        
        term_printf(term, "  Region %d: 0x%llx-0x%llx (%llu MB) [%s]\n",
                   i,
                   g_pmem.regions[i].base,
                   g_pmem.regions[i].base + g_pmem.regions[i].length,
                   g_pmem.regions[i].length / (1024 * 1024),
                   type_str);
    }
}

void pmem_dump_map(void) {
    if (!term) return;
    
    term_printf(term, "\n=== Physical Memory Map (first 1024 pages) ===\n");
    
    for (uint64_t i = 0; i < 1024 && i < g_pmem.total_pages; i++) {
        if (i % 64 == 0) {
            if (i > 0) term_printf(term, "\n");
            term_printf(term, "0x%08llx: ", i * PAGE_SIZE);
        }
        
        if (bitmap_test(g_pmem.bitmap, i)) {
            term_printf(term, "X"); // Занято
        } else {
            term_printf(term, "."); // Свободно
        }
    }
    term_printf(term, "\nLegend: . = free, X = used/reserved\n");
}
