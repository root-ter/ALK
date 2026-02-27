#include "cmd.h"
#include "tio.h"
#include "../sched/sched.h"
#include "../../drv/acpi/acpi.h"
#include "../time/clock.h"
#include "..//mem/mem.h"
#include "../../libc/string.h"
#include "../time/timer.h"
#include "../../drv/block/blockdev.h"
#include "term.h"
#include <stddef.h>

extern term_t* term;

extern volatile ClockTime system_clock;

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

static void cmd_help(void) {
    tio_printf("Available commands:\n");
    tio_printf("  help            - Show this help\n");
    tio_printf("  clear           - Clear screen\n");
    tio_printf("  ps              - List processes\n");
    tio_printf("  ps -l           - Detailed process list\n");
    tio_printf("  time            - Show current time\n");
    tio_printf("  reboot          - Reboot system\n");
    tio_printf("  shutdown        - Shutdown system\n");
    tio_printf("  meminfo         - Show memory information\n");
    tio_printf("  version         - Show kernel version\n");
    tio_printf("  echo <text>     - Echo text\n");
    tio_printf("  kill <pid>      - Kill process by PID\n");
    tio_printf("  tasks           - Show task list\n");
    tio_printf("  disklist        - Show disk list\n");
    tio_printf("  diskinfo <disk> - Show disk info\n");
    tio_printf("  diskread <disk> <lba> <count> - Read disk\n");
    tio_printf("  alk             - Show ALK version\n");
}

static void cmd_clear(void) {
    term_clear(term);
    if (term_is_prompt_enabled(term)) {
        tio_printf("> ");
    }
}

static void cmd_ps_simple(void) {
    task_info_t tasks[32];
    int count = task_list(tasks, 32);
    
    if (count == 0) {
        tio_printf("No tasks running\n");
        return;
    }
    
    tio_printf(" PID  STATE  NAME\n");
    tio_printf("==== ====== ================\n");
    
    for (int i = 0; i < count; i++) {
        const char* state_str;
        switch (tasks[i].state) {
            case TASK_RUNNING: state_str = "RUN"; break;
            case TASK_READY: state_str = "RDY"; break;
            case TASK_BLOCKED: state_str = "BLK"; break;
            case TASK_ZOMBIE: state_str = "ZOM"; break;
            default: state_str = "UNK"; break;
        }
        
        tio_printf("%4d  %-4s  %s\n", 
                   tasks[i].pid, state_str, tasks[i].name);
    }
}

static void cmd_ps_detailed(void) {
    task_info_t tasks[32];
    int count = task_list(tasks, 32);
    
    if (count == 0) {
        tio_printf("No tasks running\n");
        return;
    }
    
    tio_printf(" PID  STATE     NAME             REGS     STACK   NEXT\n");
    tio_printf("==== ======== ================ ======== ======== ========\n");
    
    for (int i = 0; i < count; i++) {
        const char* state_str;
        switch (tasks[i].state) {
            case TASK_RUNNING: state_str = "RUNNING  "; break;
            case TASK_READY: state_str = "READY    "; break;
            case TASK_BLOCKED: state_str = "BLOCKED  "; break;
            case TASK_ZOMBIE: state_str = "ZOMBIE   "; break;
            default: state_str = "UNKNOWN  "; break;
        }
        
        // Получаем текущую задачу для дополнительной информации
        task_t* current = get_current_task();
        const char* current_mark = (current && current->pid == tasks[i].pid) ? "*" : " ";
        
        tio_printf("%4d%s %s %-16s",
                   tasks[i].pid, current_mark, state_str, tasks[i].name);
        
        // Для более подробной информации нужен доступ к структуре task_t
        // Показываем базовую информацию
        tio_printf(" %08X %08X\n", 
                   tasks[i].pid * 0x1000,  // Заглушка для адреса регистров
                   tasks[i].pid * 0x2000); // Заглушка для стека
    }
    
    tio_printf("\nTotal: %d task(s)\n", count);
    tio_printf("Note: * = current task\n");
}

static void cmd_time(void) {
    char time_str[9];
    format_clock(time_str, system_clock);
    tio_printf("Current time: %s\n", time_str);
}

static void cmd_reboot(void) {
    tio_printf("Rebooting system...\n");
    acpi_reboot();
    // Если ACPI не сработал, пробуем 8042 контроллер
    asm volatile("outb %%al, %%dx" : : "a"((uint8_t)0xFE), "d"((uint16_t)0x64));
    while(1);
}

static void cmd_shutdown(void) {
    tio_printf("Shutting down...\n");
    acpi_shutdown();
    // Если ACPI не сработал, просто останавливаем
    tio_printf("ACPI shutdown failed. System halted.\n");
    while(1) asm volatile("hlt");
}

static void cmd_meminfo(void) {
    kmalloc_stats_t stats;
    get_kmalloc_stats(&stats);
    
    tio_printf("Kernel Heap Statistics:\n");
    tio_printf("  Total managed:   %lu bytes\n", stats.total_managed);
    tio_printf("  Used payload:    %lu bytes\n", stats.used_payload);
    tio_printf("  Free payload:    %lu bytes\n", stats.free_payload);
    tio_printf("  Largest free:    %lu bytes\n", stats.largest_free);
    tio_printf("  Number of blocks:%lu\n", stats.num_blocks);
    tio_printf("  Used blocks:     %lu\n", stats.num_used);
    tio_printf("  Free blocks:     %lu\n", stats.num_free);
    
    uint64_t total_memory = mb2_get_usable_memory();
    tio_printf("\nSystem Memory:\n");
    tio_printf("  Total RAM:       %lu MB\n", total_memory / (1024 * 1024));
    tio_printf("  Used by kernel:  %lu KB\n", stats.total_managed / 1024);
}

static void cmd_version(void) {
    tio_printf("ALK Kernel Version 0.03\n");
    tio_printf("Built: %s %s\n", __DATE__, __TIME__);
    tio_printf("Architecture: x86_64\n");
    tio_printf("Author: 13-year-old kernel developer\n");
    tio_printf("Features:\n");
    tio_printf("  - 64-bit protected mode\n");
    tio_printf("  - Multiboot2 compliant\n");
    tio_printf("  - ACPI support\n");
    tio_printf("  - PCI/PCIe scanning\n");
    tio_printf("  - AHCI SATA driver\n");
    tio_printf("  - Cooperative multitasking\n");
    tio_printf("  - Framebuffer console\n");
}

static void cmd_echo(char* args) {
    if (args && args[0] != '\0') {
        tio_printf("%s\n", args);
    } else {
        tio_printf("\n");
    }
}

static void cmd_kill(char* args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: kill <pid>\n");
        return;
    }
    
    int pid = atoi(args);
    if (pid < 0) {
        tio_printf("Invalid PID: %s\n", args);
        return;
    }
    
    // Нельзя убить PID 0 (ядро)
    if (pid == 0) {
        tio_printf("Cannot kill kernel process (PID 0)\n");
        return;
    }
    
    int result = task_stop(pid);
    if (result == 0) {
        tio_printf("Process %d terminated\n", pid);
    } else {
        tio_printf("Failed to kill process %d\n", pid);
    }
}

static void cmd_tasks(void) {
    task_info_t tasks[32];
    int count = task_list(tasks, 32);
    
    tio_printf("Total tasks: %d\n", count);
    tio_printf("===============\n");
    
    for (int i = 0; i < count; i++) {
        const char* state_str;
        switch (tasks[i].state) {
            case TASK_RUNNING: state_str = "RUNNING"; break;
            case TASK_READY: state_str = "READY"; break;
            case TASK_BLOCKED: state_str = "BLOCKED"; break;
            case TASK_ZOMBIE: state_str = "ZOMBIE"; break;
            default: state_str = "UNKNOWN"; break;
        }
        
        tio_printf("%4d [%s] %s\n", 
                   tasks[i].pid, state_str, tasks[i].name);
    }
}

static void cmd_disklist(void) {
    blockdev_t* list[MAX_BLOCK_DEVS];
    int count = blockdev_get_list(list, MAX_BLOCK_DEVS);
    
    if (count == 0) {
        tio_printf("No block devices found\n");
        return;
    }
    
    tio_printf("Disk  Size       Type  Status  Name\n");
    tio_printf("----  ---------  ----  ------  --------\n");
    
    for (int i = 0; i < count; i++) {
        char size_str[32];
        uint64_t size_mb = list[i]->total_bytes / (1024 * 1024);
        
        snprintf(size_str, sizeof(size_str), "%luMB", 
                (unsigned long)size_mb);
        
        const char* type_str = "Unknown";
        switch (list[i]->type) {
            case BLOCKDEV_TYPE_IDE: type_str = "IDE"; break;
            case BLOCKDEV_TYPE_AHCI: type_str = "AHCI"; break;
            case BLOCKDEV_TYPE_RAMDISK: type_str = "RAM"; break;
        }
        
        const char* status_str = "Unknown";
        switch (list[i]->status) {
            case BLOCKDEV_READY: status_str = "Ready"; break;
            case BLOCKDEV_ERROR: status_str = "Error"; break;
            case BLOCKDEV_NO_MEDIA: status_str = "NoMedia"; break;
            case BLOCKDEV_UNINITIALIZED: status_str = "Uninit"; break;
        }
        
        tio_printf("%4d  %-9s  %-4s  %-6s  %s\n",
                   i + 1, size_str, type_str, status_str, list[i]->name);
    }
}

static void cmd_diskinfo(char* args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: diskinfo <disk_name>\n");
        tio_printf("Example: diskinfo dsk_1\n");
        return;
    }
    
    // Убираем пробелы
    while (*args == ' ') args++;
    
    tio_printf("Looking for device: '%s'\n", args);
    
    blockdev_t* dev = blockdev_find(args);
    if (!dev) {
        tio_printf("Device '%s' not found!\n", args);
        tio_printf("Available devices:\n");
        
        blockdev_t* list[16];
        int count = blockdev_get_list(list, 16);
        for (int i = 0; i < count; i++) {
            tio_printf("  %s\n", list[i]->name);
        }
        return;
    }
    
    // Проверяем, что устройство готово
    if (dev->status != BLOCKDEV_READY) {
        tio_printf("Device '%s' is not ready (status: %d)\n", 
                    args, dev->status);
        return;
    }
    
    // БЕЗОПАСНО выводим информацию
    tio_printf("\n=== Device: %s ===\n", dev->name);
    tio_printf("Type: %d\n", dev->type);
    tio_printf("Status: %d\n", dev->status);
    tio_printf("Sector size: %lu bytes\n", (unsigned long)dev->sector_size);
    tio_printf("Total sectors: %lu\n", (unsigned long)dev->total_sectors);
    tio_printf("Total size: %lu MB\n", 
                (unsigned long)(dev->total_bytes / (1024 * 1024)));
    tio_printf("LBA48: %s\n", dev->supports_lba48 ? "Yes" : "No");
    
    // Если есть read/write функции - проверяем что они не NULL
    tio_printf("Read handler: %s\n", dev->read_sectors ? "OK" : "MISSING!");
    tio_printf("Write handler: %s\n", dev->write_sectors ? "OK" : "MISSING!");
    
    // Статистика
    tio_printf("Reads: %lu\n", (unsigned long)dev->read_count);
    tio_printf("Writes: %lu\n", (unsigned long)dev->write_count);
    tio_printf("Errors: %lu\n", (unsigned long)dev->error_count);
}

static void cmd_diskread(char* args) {
    // Формат: diskread <device> <lba> <count>
    // Пример: diskread dsk_1 0 1
    
    if (!args || args[0] == '\0') {
        tio_printf("Usage: diskread <device> <lba> <count>\n");
        return;
    }
    
    // Парсим аргументы
    char* saveptr;
    char* dev_name = strtok_r(args, " ", &saveptr);
    char* lba_str = strtok_r(NULL, " ", &saveptr);
    char* count_str = strtok_r(NULL, " ", &saveptr);
    
    if (!dev_name || !lba_str || !count_str) {
        tio_printf("Usage: diskread <device> <lba> <count>\n");
        return;
    }
    
    blockdev_t* dev = blockdev_find(dev_name);
    if (!dev) {
        tio_printf("Device '%s' not found\n", dev_name);
        return;
    }
    
    uint64_t lba = atol(lba_str);
    uint32_t count = atoi(count_str);
    
    if (count == 0 || count > 256) {
        tio_printf("Count must be 1-256\n");
        return;
    }
    
    // Выделяем буфер
    uint32_t buffer_size = count * dev->sector_size;
    uint8_t* buffer = (uint8_t*)malloc(buffer_size);
    
    if (!buffer) {
        tio_printf("Memory allocation failed\n");
        return;
    }
    
    tio_printf("Reading %u sectors from LBA %lu...\n", count, lba);
    
    int result = blockdev_read(dev, lba, count, buffer);
    
    if (result == 0) {
        tio_printf("Read successful\n");
        
        // Выводим первые 16 байт в hex
        tio_printf("First 16 bytes: ");
        for (int i = 0; i < 16 && i < buffer_size; i++) {
            tio_printf("%02X ", buffer[i]);
        }
        tio_printf("\n");
    } else {
        tio_printf("Read failed\n");
    }
    
    free(buffer);
}

void cmd_alk(void) {
    tio_printf("\n");
    tio_printf("     _____ __     __   __   ____  ____\n");
    tio_printf("    / __  |  |   |  | / /  /    \\| ___|\n");
    tio_printf("   / /__| |  |   |  |/ /   | /\\ |||___\n");
    tio_printf("  /  __   |  |   |     |   | || ||___ |\n");
    tio_printf(" /  /  |  |  |___|  |\\ \\   | \\/ | __| |\n");
    tio_printf("/__/   |__|______|__| \\_\\  \\____/|____|\n");
    tio_printf("\n");
    tio_printf("ALK OS v0.03\n");
    tio_printf("Shell:\n");
    tio_printf("  ALKShell\n");
    tio_printf("\nWe hope you have a good experience by using ALK :)\n");
}

// ==================== ОСНОВНАЯ ФУНКЦИЯ ОБРАБОТКИ КОМАНД ====================

int term_execute_command(char* cmdline) {
    if (!cmdline || cmdline[0] == '\0') {
        return 0;
    }
    
    // Разделяем команду и аргументы
    char cmd[64];
    char* args = NULL;
    
    // Копируем команду (первые 63 символа)
    int i = 0;
    while (cmdline[i] != ' ' && cmdline[i] != '\0' && i < 63) {
        cmd[i] = cmdline[i];
        i++;
    }
    cmd[i] = '\0';
    
    // Пропускаем пробелы и находим аргументы
    while (cmdline[i] == ' ') i++;
    if (cmdline[i] != '\0') {
        args = &cmdline[i];
    }
    
    // Обрабатываем команду
    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
    } else if (strcmp(cmd, "ps") == 0) {
        if (args && strcmp(args, "-l") == 0) {
            cmd_ps_detailed();
        } else {
            cmd_ps_simple();
        }
    } else if (strcmp(cmd, "time") == 0) {
        cmd_time();
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(cmd, "shutdown") == 0) {
        cmd_shutdown();
    } else if (strcmp(cmd, "meminfo") == 0) {
        cmd_meminfo();
    } else if (strcmp(cmd, "version") == 0) {
        cmd_version();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(args);
    } else if (strcmp(cmd, "kill") == 0) {
        cmd_kill(args);
    } else if (strcmp(cmd, "tasks") == 0) {
        cmd_tasks();
    } else if (strcmp(cmd, "disklist") == 0) {
    	cmd_disklist();
    } else if (strcmp(cmd, "diskinfo") == 0) {
    	cmd_diskinfo(args);
    } else if (strcmp(cmd, "diskread") == 0) {
    	cmd_diskread(args); 
    } else if (strcmp(cmd, "alk") == 0) {
        cmd_alk();
    } else {
        tio_printf("Unknown command: %s\n", cmd);
        tio_printf("Type 'help' for available commands\n");
        return -1;
    }
    
    return 0;
}

// ==================== ИНИЦИАЛИЗАЦИЯ КОМАНД ====================

void term_commands_init(void) {
    tio_printf("Command processor initialized\n");
}

// Функция для обработки ввода из прерывания клавиатуры
void term_handle_command_input(char input_char) {
    static char cmd_buffer[256];
    static int cmd_pos = 0;
    
    switch (input_char) {
        case '\n':
        case '\r':
            if (cmd_pos > 0) {
                cmd_buffer[cmd_pos] = '\0';
                term_printf(term, "\n");
                
                // Выполняем команду
                term_execute_command(cmd_buffer);
                
                // Сбрасываем буфер
                cmd_pos = 0;
                
                // Показываем промпт снова
                if (term_is_prompt_enabled(term)) {
                    tio_printf("> ");
                }
            } else {
                // Просто новая строка
                tio_printf("\n");
                if (term_is_prompt_enabled(term)) {
                    tio_printf("> ");
                }
            }
            break;
            
        case '\b':
            if (cmd_pos > 0) {
                cmd_pos--;
                // Нужно также обновить отображение
                // Это делает term_handle_input в терминале
            }
            break;
            
        case 0x03:  // Ctrl+C
            cmd_pos = 0;
            term_printf(term, "^C\n");
            if (term_is_prompt_enabled(term)) {
                tio_printf("> ");
            }
            break;
            
        default:
            if (input_char >= 32 && input_char < 127 && cmd_pos < 255) {
                cmd_buffer[cmd_pos++] = input_char;
            }
            break;
    }
    
    // Также передаём символ в терминал для отображения
    // (но не возвращаем строку, так как мы сами обрабатываем команды)
    char* dummy = NULL;
    term_handle_input(term, input_char, &dummy);
}
