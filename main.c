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
#include "drv/usb/usbdev/usbdev.h"
#include "drv/usb/ehci/ehci.h"
#include "base/mem/pmm.h"
#include "base/mem/paging.h"
#include "base/term/tio.h"
#include "drv/disk/ahci.h"
#include "fs/vfs/vfs.h"
#include "fs/exfat/exfat.h"
#include "fs/devfs/devfs.h"

/* символы из link.ld */
extern char _heap_start;
extern char _heap_end;

// Глобальные переменные
extern void acpi_monitor_power_button(void);
extern bool input_waiting;
framebuffer_t fb;
ide_disk_t disks[4];
pmm_t pmm;
term_t* term;
vfs_inode_t *fs_root = NULL;
blockdev_t *fs_disk = NULL;
int term_pid = 0;

static void terminal_thread(void);

static void termsaver_recovery(void) {
    // waiting 10 sec
    wait(10);
    task_create(terminal_thread, KSTACK_SIZE * 2, "ALKShell");
    task_exit(1);
}

static void termsaver(void) {
    while (task_is_alive(term_pid)) {
        asm volatile ("hlt");
    }
    // Terminal is terminated
    while (!task_is_alive(term_pid)) {
        fb_fill_rect(&fb, 
                 term->x, term->y,
                 term->cols * term->char_width,
                 term->rows * term->char_height,
                 COLOR_BLACK);
        fb_set_color(&fb, COLOR_RED, COLOR_BLACK);
        fb_printf(&fb, "TERMINAL WAS TERMINATED!\n");
        fb_printf(&fb, "TRYING TO RECOVERY...\n");

        task_create(termsaver_recovery, 0, "RECOVERY");
        
        mwait(50);

        fb_fill_rect(&fb, 
                 term->x, term->y,
                 term->cols * term->char_width,
                 term->rows * term->char_height,
                 COLOR_RED);
        fb_set_color(&fb, COLOR_BLACK, COLOR_RED);
        fb_printf(&fb, "TERMINAL WAS TERMINATED!\n");
        fb_printf(&fb, "TRYING TO RECOVERY...\n");
    }
}

static void terminal_thread(void) {

    term_clear(term);

    term_pid = get_current_task()->pid;
    
    tio_printf("\n[TERM] Terminal started (PID: %d)\n", 
               term_pid);
    
    // Включаем промпт
    term_enable_prompt(term);
    term_set_prompt_text(term, "> ");
    
    // Бесконечный цикл терминала
    while (1) {
        // Обрабатываем ввод из буфера
        term_process_input(term);
        
        // Небольшая пауза, чтобы не грузить CPU
        asm volatile("pause");
        // или mwait(1); если есть
    }
}

void zombie_reaper_task(void)
{
    for (;;)
    {
        reap_zombies();
        asm volatile("hlt");
    }
}
 
void show_alk_logo(void) {
    tio_printf("\n");
    tio_printf("\n");
    tio_printf("     _____ __     __   __   ____  ____\n");
    mwait(50);
    tio_printf("    / __  |  |   |  | / /  /    \\| ___|\n");
    mwait(50);
    tio_printf("   / /__| |  |   |  |/ /   | /\\ |||___\n");
    mwait(50);
    tio_printf("  /  __   |  |   |     |   | || ||___ |\n");
    mwait(50);
    tio_printf(" /  /  |  |  |___|  |\\ \\   | \\/ | __| |\n");
    mwait(50);
    tio_printf("/__/   |__|______|__| \\_\\  \\____/|____|\n");
    tio_printf("\n");
}

void fs_init(void) {
    tio_printf("\n[FS] Initializing filesystem layer...\n");
    
    // 1. Инициализируем VFS
    vfs_init();
    
    // 2. Регистрируем exFAT
    exfat_init();
    
    tio_printf("[FS] Ready\n");
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
    input_waiting = false;

    mb2_parse(mb2_addr);
    if(!fb_init(&fb)) {
        for (;;) asm volatile ("hlt");
    }
    
    pmm_init(&pmm, mb2_addr);
    
    size_t heap_size = (size_t)((uintptr_t)&_heap_end - (uintptr_t)&_heap_start);
    malloc_init(&_heap_start, heap_size);
    
    uint64_t total_ram = get_total_memory();
    paging_init(total_ram);

    fb_alloc_backbuffer(&fb);
    fb_enable_vsync(&fb, 60);

    scheduler_init();

    uint32_t cols = fb.width / (FONT_WIDTH + 1);
    uint32_t rows = fb.height / (FONT_HEIGHT + 2);
    term = term_init(&fb, 0, 0, cols, rows);
    tio_init(term);
    fb_fill_rect(&fb, 0, 0, fb.width, fb.height, term->bg_color);
    
    tio_printf("[TERM] Initialized\n");

    task_create(zombie_reaper_task, 0, "ZombieReap");
    tio_printf("[SCHEDULER] Created task: 'ZombieReap'\n");

    task_create(terminal_thread, KSTACK_SIZE * 2, "ALKShell");
    task_create(termsaver, 0, "ShellSave");

    pci_init();
    tio_printf("[PCI] Initialized\n");

    pc_speaker_init();

    if (acpi_init()) {
        tio_printf("[ACPI] Found %d CPU(s)\n", acpi_get_cpu_count());
    } else {
	rsod("ACPI_INIT_FAILED", "ACPI");
    }

    tio_printf("Initializing disk controllers...\n");

    for (int ch = 0; ch < 2; ch++) {
        for (int dr = 0; dr < 2; dr++) {
            int idx = ch * 2 + dr;
            
            tio_printf("Initializing %s/%s... ",
                   ch == 0 ? "Primary" : "Secondary",
                   dr == 0 ? "Master" : "Slave");
            
            int result = ide_init(&disks[idx], (ide_channel_t)ch, dr);
            
            if (result == IDE_OK) {
                tio_printf("OK (Type: %s)\n",
                       disks[idx].type == IDE_TYPE_ATA ? "ATA" : 
                       disks[idx].type == IDE_TYPE_ATAPI ? "ATAPI" : "NONE");
            } else {
                tio_printf("Failed (%d)\n", result);
            }
        }
    }

    tio_printf("Initializing block device layer...\n");
    blockdev_init();

    blockdev_scan_all_disks(1);

    if (ahci_init() == 0) {
	blockdev_scan_all_disks(2);
    } else {
	tio_printf("[AHCI] Failed.\n");
    }

    usb_core_init(term);
    ehci_init(term, &pmm);

    fs_init();
    devfs_init();
        
    /* Разрешаем прерывания */
    asm volatile("sti");

    mwait(200);
    term_clear(term);
    show_alk_logo();
    
    can_type = true;

    for (;;)
    {
        asm volatile("hlt");
    }
}