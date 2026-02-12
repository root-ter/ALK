#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "drv/io/io.h"
#include "base/int/idt.h"
#include "base/time/timer.h"
#include "base/time/clock.h"
#include "base/mem/mem.h"
#include "libc/string.h"
#include "base/sched/sched.h"
#include "base/mb2/mb2.h"
#include "drv/fb/fb.h"
#include "base/term/term.h"
#include "drv/pci/pci.h"
#include "base/rsod/rsod.h"
#include "drv/sound/pcs/pcs.h"
#include "drv/kbd/kbd.h"
#include "drv/acpi/acpi.h"
#include "drv/disk/ide.h"
#include "drv/block/blockdev.h"
#include "drv/usb/ehci.h"
#include "base/mem/pmm.h"


/* символы из link.ld */
extern char _heap_start;
extern char _heap_end;

// Глобальные переменные
extern void acpi_monitor_power_button(void);
term_t* term;
framebuffer_t fb;
ide_disk_t disks[4];
pmm_t pmm;

void zombie_reaper_task(void)
{
    for (;;)
    {
        reap_zombies();
        asm volatile("hlt");
    }
}
 
void show_alk_logo(term_t* term) {
    term_printf(term, "\n");
    term_printf(term, "\n");
    term_printf(term, "     _____ __     __   __   ____  ____\n");
    mwait(100);
    term_printf(term, "    / __  |  |   |  | / /  /    \\| ___|\n");
    mwait(100);
    term_printf(term, "   / /__| |  |   |  |/ /   | /\\ |||___\n");
    mwait(100);
    term_printf(term, "  /  __   |  |   |     |   | || ||___ |\n");
    mwait(100);
    term_printf(term, " /  /  |  |  |___|  |\\ \\   | \\/ | __| |\n");
    mwait(100);
    term_printf(term, "/__/   |__|______|__| \\_\\  \\____/|____|\n");
    term_printf(term, "\n");
}

/*-------------------------------------------------------------
    Основная функция ядра
-------------------------------------------------------------*/
void kmain(uint64_t mb2_addr)
{
    idt_install();
    init_system_clock();
    init_timer(500);
    outb(0x21, 0xF0); // маска прерываний
    outb(0xA1, 0xFF);

    can_type = false;

    mb2_parse(mb2_addr);
    if(!fb_init(&fb)) {
        for (;;) asm volatile ("hlt");
    }
    
    pmm_init(&pmm, mb2_addr);
    
    size_t heap_size = (size_t)((uintptr_t)&_heap_end - (uintptr_t)&_heap_start);
    malloc_init(&_heap_start, heap_size);

    uint32_t cols = (fb.width - 40) / (FONT_WIDTH + 1);
    uint32_t rows = (fb.height - 40) / (FONT_HEIGHT + 2);
    term = term_init(&fb, 20, 20, cols, rows);
    term_printf(term, "[TERM] Initialized\n");

    scheduler_init();
    term_printf(term, "[SCHEDULER] Initialized\n");

    task_create(zombie_reaper_task, 0, "ZombieReap");
    term_printf(term, "[SCHEDULER] Created task: 'ZombieReap'\n");

    pci_init();
    term_printf(term, "[PCI] Initialized\n");

    pc_speaker_init();

    if (acpi_init()) {
        term_printf(term, "[ACPI] Found %d CPU(s)\n", acpi_get_cpu_count());
    } else {
	rsod("ACPI_INIT_FAILED", "ACPI");
    }

    term_printf(term, "Initializing disk controllers...\n");

    for (int ch = 0; ch < 2; ch++) {
        for (int dr = 0; dr < 2; dr++) {
            int idx = ch * 2 + dr;
            
            term_printf(term, "Initializing %s/%s... ",
                   ch == 0 ? "Primary" : "Secondary",
                   dr == 0 ? "Master" : "Slave");
            
            int result = ide_init(&disks[idx], (ide_channel_t)ch, dr);
            
            if (result == IDE_OK) {
                term_printf(term, "OK (Type: %s)\n",
                       disks[idx].type == IDE_TYPE_ATA ? "ATA" : 
                       disks[idx].type == IDE_TYPE_ATAPI ? "ATAPI" : "NONE");
            } else {
                term_printf(term, "Failed (%d)\n", result);
            }
        }
    }

    term_printf(term, "Initializing block device layer...\n");
    blockdev_init();

    blockdev_scan_all_disks();

    /* Разрешаем прерывания */
    asm volatile("sti");

    show_alk_logo(term);
    
    can_type = true;
    term_enable_prompt(term);
    term_set_prompt_text(term, "> ");
    for (;;)
    {
        asm volatile("hlt");
    }
}
