#include "pmm.h"
#include "../../libc/string.h"
#include "../../base/mb2/mb2.h"
#include "../../base/term/term.h"

extern term_t* term;
static uint8_t pmm_bitmap[32 * 1024 * 1024]; // 32MB битмап = 256GB памяти
pmm_t g_pmm;

void pmm_init(pmm_t* pmm, uint64_t mb2_addr) {
    // 1. Парсим MB2
    mem_region_t regions[64];
    int region_count = mb2_get_memory_regions(regions, 64);
    
    // 2. Находим САМЫЙ НИЗКИЙ и САМЫЙ ВЫСОКИЙ адрес доступной памяти
    uint64_t lowest_addr = 0xFFFFFFFFFFFFFFFF;
    uint64_t highest_addr = 0;
    
    for (int i = 0; i < region_count; i++) {   
        if (regions[i].type == MB2_MEMORY_AVAILABLE) {
            if (regions[i].base < lowest_addr)
                lowest_addr = regions[i].base;
            
            uint64_t region_end = regions[i].base + regions[i].length;
            if (region_end > highest_addr)
                highest_addr = region_end;
        }
    }
    
    // 3. Выравниваем до границы страницы
    if (lowest_addr & (PAGE_SIZE - 1))
        lowest_addr = (lowest_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    if (highest_addr & (PAGE_SIZE - 1))
        highest_addr = highest_addr & ~(PAGE_SIZE - 1);
    
    // 4. Инициализируем структуру
    pmm->bitmap = pmm_bitmap;
    pmm->base_addr = lowest_addr;
    pmm->total_pages = (highest_addr - lowest_addr) / PAGE_SIZE;
    pmm->used_pages = 0;
    
    // 5. Обнуляем битмап
    uint32_t bitmap_bytes = (pmm->total_pages + 7) / 8;
    memset(pmm->bitmap, 0, bitmap_bytes);
    
    // 6. РЕЗЕРВИРУЕМ ВСЁ, ЧТО НЕЛЬЗЯ ТРОГАТЬ
    // Сначала помечаем ВСЕ страницы как занятые
    memset(pmm->bitmap, 0xFF, bitmap_bytes);
    pmm->used_pages = pmm->total_pages;
    
    // 7. Освобождаем ТОЛЬКО доступную память из MB2
    for (int i = 0; i < region_count; i++) {
        if (regions[i].type == MB2_MEMORY_AVAILABLE) {
            uint64_t start = regions[i].base;
            uint64_t end = regions[i].base + regions[i].length;
            
            // Выравниваем
            start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            end = end & ~(PAGE_SIZE - 1);
            
            for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
                if (addr >= lowest_addr && addr < highest_addr) {
                    uint32_t page_idx = (addr - pmm->base_addr) / PAGE_SIZE;
                    if (page_idx < pmm->total_pages) {
                        uint32_t byte_idx = page_idx / 8;
                        uint32_t bit = page_idx % 8;
                        pmm->bitmap[byte_idx] &= ~(1 << bit);
                        pmm->used_pages--;
                    }
                }
            }
        }
    }
    
    // 8. Резервируем ОБЯЗАТЕЛЬНЫЕ области:
    //    - Первые 1MB (BIOS, boot.asm)
    //    - Само ядро (_kernel_start - _kernel_end)
    //    - Multiboot2 структуры
    //    - PMM битмап
    
    extern char _kernel_start, _kernel_end;
    
    // Первые 1MB
    for (uint64_t addr = 0; addr < 0x100000; addr += PAGE_SIZE) {
        if (addr >= lowest_addr && addr < highest_addr) {
            uint32_t page_idx = (addr - pmm->base_addr) / PAGE_SIZE;
            if (page_idx < pmm->total_pages) {
                uint32_t byte_idx = page_idx / 8;
                uint32_t bit = page_idx % 8;
                if (!(pmm->bitmap[byte_idx] & (1 << bit))) {
                    pmm->bitmap[byte_idx] |= (1 << bit);
                    pmm->used_pages++;
                }
            }
        }
    }
    
    // Ядро
    for (uint64_t addr = (uint64_t)&_kernel_start; 
         addr < (uint64_t)&_kernel_end; 
         addr += PAGE_SIZE) {
        if (addr >= lowest_addr && addr < highest_addr) {
            uint32_t page_idx = (addr - pmm->base_addr) / PAGE_SIZE;
            if (page_idx < pmm->total_pages) {
                uint32_t byte_idx = page_idx / 8;
                uint32_t bit = page_idx % 8;
                if (!(pmm->bitmap[byte_idx] & (1 << bit))) {
                    pmm->bitmap[byte_idx] |= (1 << bit);
                    pmm->used_pages++;
                }
            }
        }
    }
    
    // Multiboot2 структуры
    uint64_t mb2_start = mb2_addr & ~(PAGE_SIZE - 1);
    uint32_t mb2_size = *(uint32_t*)mb2_addr; // total_size в начале
    uint64_t mb2_end = (mb2_addr + mb2_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    for (uint64_t addr = mb2_start; addr < mb2_end; addr += PAGE_SIZE) {
        if (addr >= lowest_addr && addr < highest_addr) {
            uint32_t page_idx = (addr - pmm->base_addr) / PAGE_SIZE;
            if (page_idx < pmm->total_pages) {
                uint32_t byte_idx = page_idx / 8;
                uint32_t bit = page_idx % 8;
                if (!(pmm->bitmap[byte_idx] & (1 << bit))) {
                    pmm->bitmap[byte_idx] |= (1 << bit);
                    pmm->used_pages++;
                }
            }
        }
    }
    
    // PMM битмап (сам себя)
    uint64_t bitmap_start = (uint64_t)pmm->bitmap;
    uint64_t bitmap_end = bitmap_start + bitmap_bytes;
    
    for (uint64_t addr = bitmap_start; addr < bitmap_end; addr += PAGE_SIZE) {
        if (addr >= lowest_addr && addr < highest_addr) {
            uint32_t page_idx = (addr - pmm->base_addr) / PAGE_SIZE;
            if (page_idx < pmm->total_pages) {
                uint32_t byte_idx = page_idx / 8;
                uint32_t bit = page_idx % 8;
                if (!(pmm->bitmap[byte_idx] & (1 << bit))) {
                    pmm->bitmap[byte_idx] |= (1 << bit);
                    pmm->used_pages++;
                }
            }
        }
    }
}

void* pmm_alloc_page(pmm_t* pmm) {
    if (!pmm || !pmm->bitmap) return NULL;
    
    uint32_t bitmap_bytes = (pmm->total_pages + 7) / 8;
    
    for (uint32_t byte_idx = 0; byte_idx < bitmap_bytes; byte_idx++) {
        if (pmm->bitmap[byte_idx] == 0x00) {
            // Весь байт свободен - ищем первый бит
            pmm->bitmap[byte_idx] = 0x01;
            uint32_t page_idx = byte_idx * 8;
            
            if (page_idx < pmm->total_pages) {
                pmm->used_pages++;
                uint64_t addr = pmm->base_addr + (page_idx * PAGE_SIZE);
                return (void*)addr;
            }
        }
        
        if (pmm->bitmap[byte_idx] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(pmm->bitmap[byte_idx] & (1 << bit))) {
                    pmm->bitmap[byte_idx] |= (1 << bit);
                    uint32_t page_idx = byte_idx * 8 + bit;
                    
                    if (page_idx < pmm->total_pages) {
                        pmm->used_pages++;
                        uint64_t addr = pmm->base_addr + (page_idx * PAGE_SIZE);
                        return (void*)addr;
                    }
                }
            }
        }
    }
    
    term_printf(term, "[PMM] OUT OF MEMORY!\n");
    return NULL;
}

void pmm_free_page(pmm_t* pmm, void* addr) {
    if (!pmm || !pmm->bitmap || !addr) return;
    
    uint64_t page_addr = (uint64_t)addr & ~(PAGE_SIZE - 1);
    
    if (page_addr < pmm->base_addr) return;
    
    uint32_t page_idx = (page_addr - pmm->base_addr) / PAGE_SIZE;
    if (page_idx >= pmm->total_pages) return;
    
    uint32_t byte_idx = page_idx / 8;
    uint32_t bit = page_idx % 8;
    
    if (pmm->bitmap[byte_idx] & (1 << bit)) {
        pmm->bitmap[byte_idx] &= ~(1 << bit);
        pmm->used_pages--;
    }
}
