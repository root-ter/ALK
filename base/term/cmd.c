#include "cmd.h"
#include "term.h"
#include "../sched/sched.h"
#include "../../drv/acpi/acpi.h"
#include "../time/clock.h"
#include "..//mem/mem.h"
#include "../../libc/string.h"
#include "../time/timer.h"
#include "../../drv/block/blockdev.h"
#include <stddef.h>

extern term_t* term;
extern volatile ClockTime system_clock;

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

static void cmd_help(void) {
    term_printf(term, "Available commands:\n");
    term_printf(term, "  help            - Show this help\n");
    term_printf(term, "  clear           - Clear screen\n");
    term_printf(term, "  ps              - List processes\n");
    term_printf(term, "  ps -l           - Detailed process list\n");
    term_printf(term, "  time            - Show current time\n");
    term_printf(term, "  reboot          - Reboot system\n");
    term_printf(term, "  shutdown        - Shutdown system\n");
    term_printf(term, "  meminfo         - Show memory information\n");
    term_printf(term, "  version         - Show kernel version\n");
    term_printf(term, "  echo <text>     - Echo text\n");
    term_printf(term, "  kill <pid>      - Kill process by PID\n");
    term_printf(term, "  tasks           - Show task list\n");
    term_printf(term, "  disklist        - Show disk list\n");
    term_printf(term, "  diskinfo <disk> - Show disk info\n");
    term_printf(term, "  diskread <disk> <lba> <count> - Read disk\n");
    term_printf(term, "  alk             - Show ALK version\n");
}

static void cmd_clear(void) {
    term_clear(term);
    if (term_is_prompt_enabled(term)) {
        term_printf(term, "> ");
    }
}

static void cmd_ps_simple(void) {
    task_info_t tasks[32];
    int count = task_list(tasks, 32);
    
    if (count == 0) {
        term_printf(term, "No tasks running\n");
        return;
    }
    
    term_printf(term, " PID  STATE  NAME\n");
    term_printf(term, "==== ====== ================\n");
    
    for (int i = 0; i < count; i++) {
        const char* state_str;
        switch (tasks[i].state) {
            case TASK_RUNNING: state_str = "RUN"; break;
            case TASK_READY: state_str = "RDY"; break;
            case TASK_BLOCKED: state_str = "BLK"; break;
            case TASK_ZOMBIE: state_str = "ZOM"; break;
            default: state_str = "UNK"; break;
        }
        
        term_printf(term, "%4d  %-4s  %s\n", 
                   tasks[i].pid, state_str, tasks[i].name);
    }
}

static void cmd_ps_detailed(void) {
    task_info_t tasks[32];
    int count = task_list(tasks, 32);
    
    if (count == 0) {
        term_printf(term, "No tasks running\n");
        return;
    }
    
    term_printf(term, " PID  STATE     NAME             REGS     STACK   NEXT\n");
    term_printf(term, "==== ======== ================ ======== ======== ========\n");
    
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
        
        term_printf(term, "%4d%s %s %-16s",
                   tasks[i].pid, current_mark, state_str, tasks[i].name);
        
        // Для более подробной информации нужен доступ к структуре task_t
        // Показываем базовую информацию
        term_printf(term, " %08X %08X\n", 
                   tasks[i].pid * 0x1000,  // Заглушка для адреса регистров
                   tasks[i].pid * 0x2000); // Заглушка для стека
    }
    
    term_printf(term, "\nTotal: %d task(s)\n", count);
    term_printf(term, "Legend: * = current task\n");
}

static void cmd_time(void) {
    char time_str[9];
    format_clock(time_str, system_clock);
    term_printf(term, "Current time: %s\n", time_str);
}

static void cmd_reboot(void) {
    term_printf(term, "Rebooting system...\n");
    acpi_reboot();
    // Если ACPI не сработал, пробуем 8042 контроллер
    asm volatile("outb %%al, %%dx" : : "a"((uint8_t)0xFE), "d"((uint16_t)0x64));
    while(1);
}

static void cmd_shutdown(void) {
    term_printf(term, "Shutting down...\n");
    acpi_shutdown();
    // Если ACPI не сработал, просто останавливаем
    term_printf(term, "ACPI shutdown failed. System halted.\n");
    while(1) asm volatile("hlt");
}

static void cmd_meminfo(void) {
    kmalloc_stats_t stats;
    get_kmalloc_stats(&stats);
    
    term_printf(term, "Kernel Heap Statistics:\n");
    term_printf(term, "  Total managed:   %u bytes\n", stats.total_managed);
    term_printf(term, "  Used payload:    %u bytes\n", stats.used_payload);
    term_printf(term, "  Free payload:    %u bytes\n", stats.free_payload);
    term_printf(term, "  Largest free:    %u bytes\n", stats.largest_free);
    term_printf(term, "  Number of blocks:%u\n", stats.num_blocks);
    term_printf(term, "  Used blocks:     %u\n", stats.num_used);
    term_printf(term, "  Free blocks:     %u\n", stats.num_free);
    
    uint64_t total_memory = mb2_get_usable_memory();
    term_printf(term, "\nSystem Memory:\n");
    term_printf(term, "  Total RAM:       %llu MB\n", total_memory / (1024 * 1024));
    term_printf(term, "  Used by kernel:  %u KB\n", stats.total_managed / 1024);
}

static void cmd_version(void) {
    term_printf(term, "ALK Kernel Version 0.02\n");
    term_printf(term, "Built: %s %s\n", __DATE__, __TIME__);
    term_printf(term, "Architecture: x86_64\n");
    term_printf(term, "Author: 13-year-old kernel developer\n");
    term_printf(term, "Features:\n");
    term_printf(term, "  - 64-bit protected mode\n");
    term_printf(term, "  - Multiboot2 compliant\n");
    term_printf(term, "  - ACPI support\n");
    term_printf(term, "  - PCI/PCIe scanning\n");
    term_printf(term, "  - AHCI SATA driver\n");
    term_printf(term, "  - Cooperative multitasking\n");
    term_printf(term, "  - Framebuffer console\n");
}

static void cmd_echo(char* args) {
    if (args && args[0] != '\0') {
        term_printf(term, "%s\n", args);
    } else {
        term_printf(term, "\n");
    }
}

static void cmd_kill(char* args) {
    if (!args || args[0] == '\0') {
        term_printf(term, "Usage: kill <pid>\n");
        return;
    }
    
    int pid = atoi(args);
    if (pid <= 0) {
        term_printf(term, "Invalid PID: %s\n", args);
        return;
    }
    
    // Нельзя убить PID 0 (ядро)
    if (pid == 0) {
        term_printf(term, "Cannot kill kernel process (PID 0)\n");
        return;
    }
    
    int result = task_stop(pid);
    if (result == 0) {
        term_printf(term, "Process %d terminated\n", pid);
    } else {
        term_printf(term, "Failed to kill process %d (not found)\n", pid);
    }
}

static void cmd_tasks(void) {
    task_info_t tasks[32];
    int count = task_list(tasks, 32);
    
    term_printf(term, "Total tasks: %d\n", count);
    term_printf(term, "===============\n");
    
    for (int i = 0; i < count; i++) {
        const char* state_str;
        switch (tasks[i].state) {
            case TASK_RUNNING: state_str = "RUNNING"; break;
            case TASK_READY: state_str = "READY"; break;
            case TASK_BLOCKED: state_str = "BLOCKED"; break;
            case TASK_ZOMBIE: state_str = "ZOMBIE"; break;
            default: state_str = "UNKNOWN"; break;
        }
        
        term_printf(term, "%4d [%s] %s\n", 
                   tasks[i].pid, state_str, tasks[i].name);
    }
}

static void cmd_disklist(void) {
    blockdev_t* list[MAX_BLOCK_DEVS];
    int count = blockdev_get_list(list, MAX_BLOCK_DEVS);
    
    if (count == 0) {
        term_printf(term, "No block devices found\n");
        return;
    }
    
    term_printf(term, "Disk  Size       Type  Status  Name\n");
    term_printf(term, "----  ---------  ----  ------  --------\n");
    
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
        
        term_printf(term, "%4d  %-9s  %-4s  %-6s  %s\n",
                   i + 1, size_str, type_str, status_str, list[i]->name);
    }
}

static void cmd_diskinfo(char* args) {
    if (!args || args[0] == '\0') {
        term_printf(term, "Usage: diskinfo <disk_name or disk_number>\n");
        term_printf(term, "Example: diskinfo dsk_1\n");
        return;
    }
    
    blockdev_t* dev = NULL;
    
    // Проверяем, является ли аргумент числом
    if (args[0] >= '0' && args[0] <= '9') {
        int num = atoi(args);
        dev = blockdev_find_by_number(num);
    } else {
        dev = blockdev_find(args);
    }
    
    if (!dev) {
        term_printf(term, "Device '%s' not found\n", args);
        return;
    }
    
    char info[512];
    blockdev_get_info(dev, info, sizeof(info));
    term_printf(term, "%s\n", info);
}

static void cmd_diskread(char* args) {
    // Формат: diskread <device> <lba> <count>
    // Пример: diskread dsk_1 0 1
    
    if (!args || args[0] == '\0') {
        term_printf(term, "Usage: diskread <device> <lba> <count>\n");
        return;
    }
    
    // Парсим аргументы
    char* saveptr;
    char* dev_name = strtok_r(args, " ", &saveptr);
    char* lba_str = strtok_r(NULL, " ", &saveptr);
    char* count_str = strtok_r(NULL, " ", &saveptr);
    
    if (!dev_name || !lba_str || !count_str) {
        term_printf(term, "Usage: diskread <device> <lba> <count>\n");
        return;
    }
    
    blockdev_t* dev = blockdev_find(dev_name);
    if (!dev) {
        term_printf(term, "Device '%s' not found\n", dev_name);
        return;
    }
    
    uint64_t lba = atol(lba_str);
    uint32_t count = atoi(count_str);
    
    if (count == 0 || count > 256) {
        term_printf(term, "Count must be 1-256\n");
        return;
    }
    
    // Выделяем буфер
    uint32_t buffer_size = count * dev->sector_size;
    uint8_t* buffer = (uint8_t*)malloc(buffer_size);
    
    if (!buffer) {
        term_printf(term, "Memory allocation failed\n");
        return;
    }
    
    term_printf(term, "Reading %u sectors from LBA %llu...\n", count, lba);
    
    int result = blockdev_read(dev, lba, count, buffer);
    
    if (result == 0) {
        term_printf(term, "Read successful\n");
        
        // Выводим первые 16 байт в hex
        term_printf(term, "First 16 bytes: ");
        for (int i = 0; i < 16 && i < buffer_size; i++) {
            term_printf(term, "%02X ", buffer[i]);
        }
        term_printf(term, "\n");
    } else {
        term_printf(term, "Read failed\n");
    }
    
    free(buffer);
}

void cmd_alk(void) {
    term_printf(term, "\n");
    term_printf(term, "     _____ __     __   __   ____  ____\n");
    term_printf(term, "    / __  |  |   |  | / /  /    \\| ___|\n");
    term_printf(term, "   / /__| |  |   |  |/ /   | /\\ |||___\n");
    term_printf(term, "  /  __   |  |   |     |   | || ||___ |\n");
    term_printf(term, " /  /  |  |  |___|  |\\ \\   | \\/ | __| |\n");
    term_printf(term, "/__/   |__|______|__| \\_\\  \\____/|____|\n");
    term_printf(term, "\n");
    term_printf(term, "ALK OS v0.02\n");
    term_printf(term, "Shell:\n");
    term_printf(term, "  ALKShell\n");
    term_printf(term, "\nWe hope you have a good experience by using ALK :)\n");
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
        term_printf(term, "Unknown command: %s\n", cmd);
        term_printf(term, "Type 'help' for available commands\n");
        return -1;
    }
    
    return 0;
}

// ==================== ИНИЦИАЛИЗАЦИЯ КОМАНД ====================

void term_commands_init(void) {
    term_printf(term, "Command processor initialized\n");
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
                    term_printf(term, "> ");
                }
            } else {
                // Просто новая строка
                term_printf(term, "\n");
                if (term_is_prompt_enabled(term)) {
                    term_printf(term, "> ");
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
                term_printf(term, "> ");
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
