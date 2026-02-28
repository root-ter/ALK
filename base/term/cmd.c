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
#include "../../fs/vfs/vfs.h"
#include "../../fs/exfat/exfat.h"
#include <stddef.h>

extern term_t* term;
extern vfs_inode_t *fs_root;
static vfs_inode_t *current_dir = NULL;
static char current_path[PATH_MAX] = "/";

extern volatile ClockTime system_clock;

static char* build_path_recursive(vfs_inode_t *inode, char *buffer, int depth) {
    if (!inode || depth > 100) return buffer;
    
    // Если это корень
    if (inode == fs_root) {
        buffer[0] = '/';
        buffer[1] = '\0';
        return buffer;
    }
    
    // Получаем родителя
    vfs_inode_t *parent = NULL;
    if (vfs_parent(inode, &parent) != 0) {
        return buffer;
    }
    
    // Рекурсивно строим путь родителя
    if (parent != inode) {  // Защита от зацикливания
        build_path_recursive(parent, buffer, depth + 1);
    }
    
    // Добавляем свое имя
    char name[256];
    if (exfat_get_name(inode, name, sizeof(name)) == 0 && name[0] != '\0') {
        int len = strlen(buffer);
        if (len > 0 && buffer[len-1] != '/') {
            strcat(buffer, "/");
        }
        strcat(buffer, name);
    }
    
    if (parent && parent != inode && parent != fs_root) {
        vfs_free_inode(parent);
    }
    
    return buffer;
}

// Обновить current_path
static void update_current_path(void) {
    if (!fs_root || !current_dir) {
        strcpy(current_path, "/");
        return;
    }
    
    char temp[PATH_MAX];
    temp[0] = '\0';
    
    if (current_dir == fs_root) {
        strcpy(current_path, "/");
    } else {
        build_path_recursive(current_dir, temp, 0);
        if (temp[0] == '\0') {
            strcpy(current_path, "/");
        } else {
            strcpy(current_path, temp);
        }
    }
}

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

static void cmd_help(void) {
    tio_printf("Available commands:\n");
    tio_printf("  help                | cd\n");
    tio_printf("  clear               | pwd\n");
    tio_printf("  ps                  |\n");
    tio_printf("  ps -l               |\n");
    tio_printf("  time                |\n");
    tio_printf("  reboot              |\n");
    tio_printf("  shutdown            |\n");
    tio_printf("  meminfo             |\n");
    tio_printf("  version             |\n");
    tio_printf("  echo <text>         |\n");
    tio_printf("  kill <pid>          |\n");
    tio_printf("  tasks               |\n");
    tio_printf("  disklist            |\n");
    tio_printf("  diskinfo <disk>     |\n");
    tio_printf("  diskread <disk> <lba> <count> |\n");
    tio_printf("  alk                 |\n");
    tio_printf("  cat <file>          |\n");
    tio_printf("  mount [num]         |\n");
    tio_printf("  exformat <num>      |\n");
    tio_printf("  touch <file>        |\n");
    tio_printf("  mkdir <dir>         |\n");
    tio_printf("  rm <path>           |\n");
    tio_printf("  write <file> <text> |\n");
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

static void cmd_mount(char *args) {
    blockdev_t *disks[16];
    int count = blockdev_get_list(disks, 16);
    
    if (count == 0) {
        tio_printf("No disks available\n");
        return;
    }
    
    // Если аргумент не указан - показываем список
    if (!args || args[0] == '\0') {
        tio_printf("Available disks:\n");
        for (int i = 0; i < count; i++) {
            char size_str[32];
            uint64_t size_mb = disks[i]->total_bytes / (1024 * 1024);
            snprintf(size_str, sizeof(size_str), "%luMB", size_mb);
            
            tio_printf("  %d: %s [%s] - %s\n", 
                       i + 1, disks[i]->name, size_str,
                       disks[i]->type == BLOCKDEV_TYPE_AHCI ? "SATA" : "IDE");
        }
        tio_printf("Usage: mount <number>\n");
        return;
    }
    
    // Парсим номер диска
    int disk_num = atoi(args);
    if (disk_num < 1 || disk_num > count) {
        tio_printf("Invalid disk number\n");
        return;
    }
    
    blockdev_t *disk = disks[disk_num - 1];
    tio_printf("Mounting %s...\n", disk->name);
    
    // Пробуем exFAT
    vfs_inode_t *root;
    if (vfs_mount("exfat", disk, &root) == 0) {
        if (fs_root) {
            // Размонтируем старую ФС
            vfs_unmount(fs_root);
        }
        fs_root = root;
	current_dir = fs_root;
        strcpy(current_path, "/");
        tio_printf("Mounted %s as exFAT\n", disk->name);
    } else {
        tio_printf("Failed to mount %s (not exFAT?)\n", disk->name);
    }
}

static void cmd_cat(char *args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: cat <filename>\n");
        return;
    }
    
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    vfs_file_t *file;
    if (vfs_open(current_dir, args, O_READ, &file) == 0) {
        char buffer[512];
        uint32_t read;
        while (vfs_read(file, buffer, 512, &read) == 0 && read > 0) {
            for (uint32_t i = 0; i < read; i++) {
                term_putc(term, buffer[i]);
            }
        }
        tio_printf("\n");
        vfs_close(file);
    } else {
        tio_printf("File not found: %s\n", args);
    }
}

static void cmd_exformat(char *args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: exformat <disk_number>\n");
        tio_printf("WARNING: This will erase ALL data on the disk!\n");
        return;
    }
    
    // Получаем список дисков
    blockdev_t *disks[16];
    int count = blockdev_get_list(disks, 16);
    
    int disk_num = atoi(args);
    if (disk_num < 1 || disk_num > count) {
        tio_printf("Invalid disk number\n");
        return;
    }
    
    blockdev_t *disk = disks[disk_num - 1];
    
    tio_printf("Formatting %s as exFAT...\n", disk->name);
    
    // Вызываем функцию форматирования
    if (exfat_format(disk) == 0) {
        tio_printf("Format successful!\n");
        
        // Автоматически монтируем после форматирования
        vfs_inode_t *root;
        if (vfs_mount("exfat", disk, &root) == 0) {
            if (fs_root) vfs_unmount(fs_root);
            fs_root = root;
            tio_printf("Mounted %s\n", disk->name);
        }
    } else {
        tio_printf("Format failed!\n");
    }
}

static void cmd_touch(char *args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: touch <filename>\n");
        return;
    }
    
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    vfs_inode_t *inode;
    if (vfs_create(current_dir, args, FT_REG_FILE, &inode) == 0) {
        tio_printf("File created: %s\n", args);
    } else {
        tio_printf("Failed to create file: %s\n", args);
    }
}

// Создание директории
static void cmd_mkdir(char *args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: mkdir <dirname>\n");
        return;
    }
    
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    vfs_inode_t *new_dir;
    if (vfs_mkdir(current_dir, args, FT_DIR, &new_dir) == 0) {
        tio_printf("Directory created: %s\n", args);
        vfs_free_inode(new_dir);  // Освобождаем, если не нужен
    } else {
        tio_printf("Failed to create directory: %s\n", args);
    }
}
// Удаление файла или директории
static void cmd_rm(char *args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: rm <path>\n");
        return;
    }
    
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    if (vfs_unlink(current_dir, args) == 0) {
        tio_printf("Removed: %s\n", args);
    } else {
        tio_printf("Failed to remove: %s\n", args);
    }
}

// Запись в файл (создает или перезаписывает)
static void cmd_write(char *args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: write <filename> <text>\n");
        return;
    }
    
    // Парсим аргументы: первое слово - имя файла, остальное - текст
    char filename[256];
    char text[256];
    char *space = strchr(args, ' ');
    
    if (!space) {
        tio_printf("Usage: write <filename> <text>\n");
        return;
    }
    
    int name_len = space - args;
    if (name_len >= 256) name_len = 255;
    memcpy(filename, args, name_len);
    filename[name_len] = '\0';
    
    strcpy(text, space + 1);
    
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    // Открываем файл для записи (создаем если нет)
    vfs_file_t *file;
    if (vfs_open(current_dir, filename, O_WRITE | O_CREAT, &file) == 0) {
        uint32_t written;
        if (vfs_write(file, text, strlen(text), &written) == 0) {
            tio_printf("Written %d bytes to %s\n", written, filename);
        } else {
            tio_printf("Write failed\n");
        }
        vfs_close(file);
    } else {
        tio_printf("Failed to open %s\n", filename);
    }
}

static void cmd_ls(char *args) {
    vfs_inode_t *target_dir;
    char *path = args;
    
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    // Если путь не указан, используем текущую директорию
    if (!path || path[0] == '\0') {
        target_dir = current_dir;
    } else {
        // Убираем ведущие пробелы
        while (*path == ' ') path++;
        
        vfs_inode_t *new_dir = NULL;
        
        // Обрабатываем путь
        if (path[0] == '/') {
            if (vfs_walk(fs_root, path, &new_dir) != 0) {
                tio_printf("ls: %s: No such file or directory\n", path);
                return;
            }
        } else {
            if (vfs_walk(current_dir, path, &new_dir) != 0) {
                tio_printf("ls: %s: No such file or directory\n", path);
                return;
            }
        }
        
        if (new_dir->i_mode != FT_DIR) {
            tio_printf("ls: %s: Not a directory\n", path);
            vfs_free_inode(new_dir);
            return;
        }
        target_dir = new_dir;
    }
    
    // Показываем путь
    if (target_dir == fs_root) {
        tio_printf("\nContents of /:\n");
    } else if (target_dir == current_dir && (!args || args[0] == '\0')) {
        tio_printf("\nContents of %s:\n", current_path);
    } else {
        tio_printf("\nContents of %s:\n", path);
    }
    
    tio_printf("%-4s %-30s %s\n", "Type", "Name", "Size");
    tio_printf("---- ------------------------------ ----------\n");
    
    uint64_t pos = 0;
    char name[256];
    uint32_t name_len;
    uint32_t type;
    int count = 0;
    
    while (vfs_readdir(target_dir, &pos, name, &name_len, &type) == 0) {
        char type_char = (type == FT_DIR) ? 'D' : 'F';
        
        if (type == FT_REG_FILE) {
            vfs_inode_t *file_inode = NULL;
            if (vfs_lookup(target_dir, name, &file_inode) == 0) {
                tio_printf("[%c]   %-30s %lu bytes\n", 
                           type_char, name, (unsigned long)file_inode->i_size);
                vfs_free_inode(file_inode);
            } else {
                tio_printf("[%c]   %-30s\n", type_char, name);
            }
        } else {
            tio_printf("[%c]   %-30s\n", type_char, name);
        }
        count++;
    }
    
    if (count == 0) {
        tio_printf("  (empty directory)\n");
    }
    tio_printf("\nTotal: %d items\n", count);
    
    // Освобождаем если это временная директория
    if (target_dir != current_dir && target_dir != fs_root) {
        vfs_free_inode(target_dir);
    }
}

static void cmd_rmdir(char *args) {
    if (!args || args[0] == '\0') {
        tio_printf("Usage: rmdir <dirname>\n");
        return;
    }
    
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    // Пытаемся удалить директорию
    if (vfs_rmdir(fs_root, args) == 0) {
        tio_printf("Directory removed: %s\n", args);
    } else {
        tio_printf("Failed to remove directory: %s (not empty or not a directory)\n", args);
    }
}

static void cmd_cd(char *args) {
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    vfs_inode_t *new_dir;
    
    // Если аргументов нет, переходим в корень
    if (!args || args[0] == '\0') {
        new_dir = fs_root;
    } else {
        // Убираем пробелы
        while (*args == ' ') args++;
        
        // Обрабатываем путь
        if (args[0] == '/') {
            // Абсолютный путь
            if (vfs_walk(fs_root, args + 1, &new_dir) != 0) {
                tio_printf("cd: %s: No such directory\n", args);
                return;
            }
        } else if (strcmp(args, "..") == 0) {
            // Переход на уровень вверх
            if (current_dir == fs_root) {
                new_dir = fs_root;
            } else {
                if (vfs_parent(current_dir, &new_dir) != 0) {
                    tio_printf("cd: ..: Cannot get parent\n");
                    return;
                }
            }
        } else {
            // Относительный путь
            char full_path[PATH_MAX];
            if (current_dir == fs_root) {
                if (snprintf(full_path, sizeof(full_path), "/%s", args) >= (int)sizeof(full_path)) {
                    tio_printf("cd: Path too long\n");
                    return;
                }
            } else {
                // Получаем полный путь через рекурсию
                char current_path_copy[PATH_MAX];
                strcpy(current_path_copy, current_path);
                
                if (current_path_copy[strlen(current_path_copy)-1] == '/') {
                    snprintf(full_path, sizeof(full_path), "%s%s", current_path_copy, args);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s/%s", current_path_copy, args);
                }
            }
            
            if (vfs_walk(fs_root, full_path + 1, &new_dir) != 0) {
                tio_printf("cd: %s: No such directory\n", args);
                return;
            }
        }
    }
    
    // Проверяем, что это директория
    if (new_dir->i_mode != FT_DIR) {
        tio_printf("cd: %s: Not a directory\n", args);
        if (new_dir != fs_root) vfs_free_inode(new_dir);
        return;
    }
    
    // Освобождаем старую директорию
    if (current_dir && current_dir != fs_root) {
        vfs_free_inode(current_dir);
    }
    
    current_dir = new_dir;
    update_current_path();
    
    // Показываем новый путь
    tio_printf("%s\n", current_path);
}

static void cmd_pwd(void) {
    if (!fs_root) {
        tio_printf("No filesystem mounted\n");
        return;
    }
    
    tio_printf("%s\n", current_path);
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
    } else if (strcmp(cmd, "mount") == 0) {
        cmd_mount(args);
    } else if (strcmp(cmd, "exformat") == 0) {
        cmd_exformat(args);
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_cat(args);
    } else if (strcmp(cmd, "ls") == 0) {
	cmd_ls(args);
    } else if (strcmp(cmd, "rmdir") == 0) {
	cmd_rmdir(args);
    } else if (strcmp(cmd, "write") == 0) {
	cmd_write(args);
    } else if (strcmp(cmd, "rm") == 0) {
	cmd_rm(args);
    } else if (strcmp(cmd, "mkdir") == 0) {
	cmd_mkdir(args);
    } else if (strcmp(cmd, "touch") == 0) {
	cmd_touch(args);
    } else if (strcmp(cmd, "cd") == 0) {
	cmd_cd(args);
    } else if (strcmp(cmd, "pwd") == 0) {
	cmd_pwd();
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
