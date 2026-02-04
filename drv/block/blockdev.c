#include "blockdev.h"
#include "../disk/ide.h"
#include "../disk/ahci.h"
#include "../../base/mem/mem.h"
#include "../../libc/string.h"
#include "../../base/term/term.h"
#include <stdarg.h>

/* Глобальные переменные */
static blockdev_t *blockdev_list_head = NULL; /* Изменено имя! */
static int blockdev_count = 0;
extern term_t* term;
extern ide_disk_t disks[4];

/* Вспомогательные функции для форматирования */
static void format_size(uint64_t bytes, char *buffer, size_t size) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size_d = (double)bytes;
    
    while (size_d >= 1024.0 && unit < 4) {
        size_d /= 1024.0;
        unit++;
    }
    
    snprintf(buffer, size, "%.2f %s", size_d, units[unit]);
}

/* Обёртки для IDE устройств */
static int ide_read_wrapper(blockdev_t *bdev, uint64_t lba, 
                           uint32_t count, void *buffer) {
    if (!bdev || !buffer || count == 0) return -1;
    
    ide_disk_t *ide_disk = (ide_disk_t*)bdev->device_data.ide.ide_disk;
    if (!ide_disk) return -1;
    
    int result = ide_read_sectors(ide_disk, lba, count, buffer);
    if (result == IDE_OK) {
        bdev->read_count++;
        return 0;
    }
    
    bdev->error_count++;
    return -1;
}

static int ide_write_wrapper(blockdev_t *bdev, uint64_t lba, 
                            uint32_t count, const void *buffer) {
    if (!bdev || !buffer || count == 0) return -1;
    
    ide_disk_t *ide_disk = (ide_disk_t*)bdev->device_data.ide.ide_disk;
    if (!ide_disk) return -1;
    
    int result = ide_write_sectors(ide_disk, lba, count, buffer);
    if (result == IDE_OK) {
        bdev->write_count++;
        return 0;
    }
    
    bdev->error_count++;
    return -1;
}

static int ide_flush_wrapper(blockdev_t *bdev) {
    /* IDE автоматически сбрасывает кэш после каждой записи */
    return 0;
}

static int ahci_read_wrapper(blockdev_t *bdev, uint64_t lba, 
                            uint32_t count, void *buffer) {
    if (!bdev || !buffer || count == 0) {
        return -1;
    }
    
    ahci_port_t *port = (ahci_port_t*)bdev->device_data.ahci.ahci_port;
    ahci_controller_t *ctrl = (ahci_controller_t*)bdev->device_data.ahci.ahci_ctrl;
    
    if (!port || !ctrl || !port->has_device) {
        bdev->error_count++;
        return -1;
    }
    
    if (lba + count > port->sectors) {
        term_printf(term, "[BLOCKDEV] AHCI read out of bounds: LBA=%llu, Count=%u, Max=%llu\n",
                   lba, count, port->sectors);
        bdev->error_count++;
        return -1;
    }
    
    bool success = ahci_read(port, lba, count, buffer);
    
    if (success) {
        bdev->read_count++;
        return 0;
    }
    
    bdev->error_count++;
    return -1;
}

static int ahci_write_wrapper(blockdev_t *bdev, uint64_t lba, 
                             uint32_t count, const void *buffer) {
    if (!bdev || !buffer || count == 0) {
        return -1;
    }
    
    ahci_port_t *port = (ahci_port_t*)bdev->device_data.ahci.ahci_port;
    ahci_controller_t *ctrl = (ahci_controller_t*)bdev->device_data.ahci.ahci_ctrl;
    
    if (!port || !ctrl || !port->has_device) {
        bdev->error_count++;
        return -1;
    }
    
    if (lba + count > port->sectors) {
        term_printf(term, "[BLOCKDEV] AHCI write out of bounds: LBA=%llu, Count=%u, Max=%llu\n",
                   lba, count, port->sectors);
        bdev->error_count++;
        return -1;
    }
    
    bool success = ahci_write(port, lba, count, buffer);
    
    if (success) {
        bdev->write_count++;
        return 0;
    }
    
    bdev->error_count++;
    return -1;
}

static int ahci_flush_wrapper(blockdev_t *bdev) {
    // AHCI автоматически сбрасывает кэш контроллера
    // При желании можно добавить команду FLUSH CACHE EXT (0xEA)
    // Но этого я делать не буду :)
    return 0;
}

/* Инициализация системы блочных устройств */
void blockdev_init(void) {
    term_printf(term, "[BLOCKDEV] Initializing block device system...\n");
    
    blockdev_list_head = NULL;
    blockdev_count = 0;
    
    term_printf(term, "[BLOCKDEV] System ready (max %d devices)\n", 
                MAX_BLOCK_DEVS);
}

blockdev_t* blockdev_register(const char *name, blockdev_type_t type) {
    /* Проверки */
    if (!name || name[0] == '\0') {
        term_printf(term, "[BLOCKDEV] Error: invalid device name\n");
        return NULL;
    }
    
    if (blockdev_count >= MAX_BLOCK_DEVS) {
        term_printf(term, "[BLOCKDEV] Error: maximum devices reached\n");
        return NULL;
    }
    
    /* Проверяем, нет ли уже устройства с таким именем */
    if (blockdev_find(name)) {
        term_printf(term, "[BLOCKDEV] Error: device '%s' already exists\n", 
                    name);
        return NULL;
    }
    
    /* Выделяем память */
    blockdev_t *dev = (blockdev_t*)malloc(sizeof(blockdev_t));
    if (!dev) {
        term_printf(term, "[BLOCKDEV] Error: memory allocation failed\n");
        return NULL;
    }
    
    /* Инициализируем структуру */
    memset(dev, 0, sizeof(blockdev_t));
    
    /* Копируем имя */
    strncpy(dev->name, name, BLOCKDEV_NAME_LEN - 1);
    dev->name[BLOCKDEV_NAME_LEN - 1] = '\0';
    
    /* Устанавливаем тип и статус */
    dev->type = type;
    dev->status = BLOCKDEV_UNINITIALIZED;
    
    /* Устанавливаем обработчики по умолчанию (будут переопределены) */
    dev->read_sectors = NULL;
    dev->write_sectors = NULL;
    dev->flush_cache = NULL;
    
    /* Добавляем в список */
    dev->next = blockdev_list_head;
    blockdev_list_head = dev;
    blockdev_count++;
    
    term_printf(term, "[BLOCKDEV] Registered device: %s (type: %d)\n", 
                name, type);
    
    return dev;
}

/* Поиск устройства по имени */
blockdev_t* blockdev_find(const char *name) {
    if (!name) return NULL;
    
    blockdev_t *dev = blockdev_list_head;
    while (dev) {
        if (strcmp(dev->name, name) == 0) {
            return dev;
        }
        dev = dev->next;
    }
    
    return NULL;
}

/* Поиск устройства по номеру */
blockdev_t* blockdev_find_by_number(int num) {
    char name[BLOCKDEV_NAME_LEN];
    snprintf(name, sizeof(name), "dsk_%d", num);
    return blockdev_find(name);
}

/* Получение списка всех устройств */
int blockdev_get_list(blockdev_t **list, int max_count) {
    if (!list || max_count <= 0) return 0;
    
    int count = 0;
    blockdev_t *dev = blockdev_list_head;
    
    while (dev && count < max_count) {
        list[count++] = dev;
        dev = dev->next;
    }
    
    return count;
}

/* Чтение секторов */
int blockdev_read(blockdev_t *dev, uint64_t lba, 
                  uint32_t count, void *buffer) {
    /* Проверки */
    if (!dev || !buffer) {
        return -1;
    }
    
    if (dev->status != BLOCKDEV_READY) {
        term_printf(term, "[BLOCKDEV] Device %s not ready for reading\n", 
                    dev->name);
        return -1;
    }
    
    if (lba + count > dev->total_sectors) {
        term_printf(term, "[BLOCKDEV] Read out of bounds on %s\n", dev->name);
        return -1;
    }
    
    if (!dev->read_sectors) {
        term_printf(term, "[BLOCKDEV] No read handler for %s\n", dev->name);
        return -1;
    }
    
    /* Вызываем обработчик */
    return dev->read_sectors(dev, lba, count, buffer);
}

/* Запись секторов */
int blockdev_write(blockdev_t *dev, uint64_t lba, 
                   uint32_t count, const void *buffer) {
    /* Проверки */
    if (!dev || !buffer) {
        return -1;
    }
    
    if (dev->status != BLOCKDEV_READY) {
        term_printf(term, "[BLOCKDEV] Device %s not ready for writing\n", 
                    dev->name);
        return -1;
    }
    
    if (lba + count > dev->total_sectors) {
        term_printf(term, "[BLOCKDEV] Write out of bounds on %s\n", dev->name);
        return -1;
    }
    
    if (!dev->write_sectors) {
        term_printf(term, "[BLOCKDEV] No write handler for %s\n", dev->name);
        return -1;
    }
    
    /* Вызываем обработчик */
    int result = dev->write_sectors(dev, lba, count, buffer);
    
    /* Если нужно, сбрасываем кэш */
    if (result == 0 && dev->flush_cache) {
        dev->flush_cache(dev);
    }
    
    return result;
}

/* Сброс кэша */
int blockdev_flush(blockdev_t *dev) {
    if (!dev || !dev->flush_cache) {
        return 0; /* Некоторые устройства не поддерживают */
    }
    
    return dev->flush_cache(dev);
}

/* Получение информации об устройстве */
void blockdev_get_info(blockdev_t *dev, char *buffer, size_t buf_size) {
    if (!dev || !buffer || buf_size == 0) {
        if (buffer && buf_size > 0) buffer[0] = '\0';
        return;
    }
    
    char size_str[32];
    char type_str[32];
    
    /* Форматируем размер */
    format_size(dev->total_bytes, size_str, sizeof(size_str));
    
    /* Определяем тип */
    switch (dev->type) {
        case BLOCKDEV_TYPE_IDE:
            strcpy(type_str, "IDE");
            break;
        case BLOCKDEV_TYPE_AHCI:
            strcpy(type_str, "AHCI");
            break;
        case BLOCKDEV_TYPE_RAMDISK:
            strcpy(type_str, "RAMDISK");
            break;
        default:
            strcpy(type_str, "UNKNOWN");
            break;
    }
    
    /* Дополнительная информация в зависимости от типа */
    char extra_info[128] = "";
    
    if (dev->type == BLOCKDEV_TYPE_AHCI) {
        ahci_port_t *port = (ahci_port_t*)dev->device_data.ahci.ahci_port;
        if (port) {
            snprintf(extra_info, sizeof(extra_info), 
                    "\nAHCI Port: %d, Speed: Gen%d",
                    dev->device_data.ahci.port_num,
                    port->sata_speed);
        }
    } else if (dev->type == BLOCKDEV_TYPE_IDE) {
        snprintf(extra_info, sizeof(extra_info),
                "\nIDE Channel: %s, Drive: %s",
                dev->device_data.ide.channel == 0 ? "Primary" : "Secondary",
                dev->device_data.ide.drive == 0 ? "Master" : "Slave");
    }
    
    /* Формируем строку */
    snprintf(buffer, buf_size, 
             "Device: %s\n"
             "Type: %s\n"
             "Status: %s\n"
             "Size: %s\n"
             "Sector size: %lu bytes\n"
             "Total sectors: %lu\n"
             "LBA48: %s\n"
             "Stats: %lu reads, %lu writes, %lu errors%s",
             dev->name,
             type_str,
             dev->status == BLOCKDEV_READY ? "READY" : 
             dev->status == BLOCKDEV_ERROR ? "ERROR" : "NOT READY",
             size_str,
             (unsigned long)dev->sector_size,
             (unsigned long)dev->total_sectors,
             dev->supports_lba48 ? "Yes" : "No",
             (unsigned long)dev->read_count,
             (unsigned long)dev->write_count,
             (unsigned long)dev->error_count,
             extra_info);
}

/* Удаление устройства */
void blockdev_unregister(blockdev_t *dev) {
    if (!dev) return;
    
    /* Удаляем из списка */
    if (blockdev_list_head == dev) {
        blockdev_list_head = dev->next;
    } else {
        blockdev_t *prev = blockdev_list_head;
        while (prev && prev->next != dev) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = dev->next;
        }
    }
    
    term_printf(term, "[BLOCKDEV] Unregistered device: %s\n", dev->name);
    
    /* Освобождаем память */
    free(dev);
    blockdev_count--;
}

/* Отладочный вывод всех устройств */
void blockdev_dump_all(void) {
    if (!term) return;
    
    term_printf(term, "\n=== Block Devices (%d total) ===\n", blockdev_count);
    
    blockdev_t *dev = blockdev_list_head;
    int index = 1;
    
    while (dev) {
        char size_str[32];
        format_size(dev->total_bytes, size_str, sizeof(size_str));
        
        const char *type_str = "UNKNOWN";
        switch (dev->type) {
            case BLOCKDEV_TYPE_IDE: type_str = "IDE"; break;
            case BLOCKDEV_TYPE_AHCI: type_str = "AHCI"; break;
            case BLOCKDEV_TYPE_RAMDISK: type_str = "RAMDISK"; break;
        }
        
        const char *status_str = "UNKNOWN";
        switch (dev->status) {
            case BLOCKDEV_UNINITIALIZED: status_str = "UNINIT"; break;
            case BLOCKDEV_READY: status_str = "READY"; break;
            case BLOCKDEV_ERROR: status_str = "ERROR"; break;
            case BLOCKDEV_NO_MEDIA: status_str = "NO MEDIA"; break;
        }
        
        // Дополнительная информация для AHCI
        char extra[32] = "";
        if (dev->type == BLOCKDEV_TYPE_AHCI) {
            snprintf(extra, sizeof(extra), "P%d", dev->device_data.ahci.port_num);
        } else if (dev->type == BLOCKDEV_TYPE_IDE) {
            snprintf(extra, sizeof(extra), "C%dD%d", 
                    dev->device_data.ide.channel,
                    dev->device_data.ide.drive);
        }
        
        term_printf(term, "%d. %-8s [%-6s] %-6s %-4s %12s  Sectors: %-8lu\n",
                   index++,
                   dev->name,
                   type_str,
                   status_str,
                   extra,
                   size_str,
                   (unsigned long)dev->total_sectors);
        
        dev = dev->next;
    }
    
    if (blockdev_count == 0) {
        term_printf(term, "No block devices found\n");
    }
}

/* ==================== ИНТЕГРАЦИЯ С СУЩЕСТВУЮЩИМИ ДРАЙВЕРАМИ ==================== */

/* Регистрация IDE диска */
void blockdev_register_ide(void *ide_disk_ptr, const char *name, 
                          uint8_t channel, uint8_t drive) {
    ide_disk_t *ide_disk = (ide_disk_t*)ide_disk_ptr;
    
    if (!ide_disk || ide_disk->type == IDE_TYPE_NONE) {
        return;
    }
    
    /* Создаём блочное устройство */
    blockdev_t *bdev = blockdev_register(name, BLOCKDEV_TYPE_IDE);
    if (!bdev) return;
    
    /* Заполняем информацию */
    bdev->sector_size = ide_disk->sector_size;
    bdev->total_sectors = ide_disk->total_sectors;
    bdev->total_bytes = ide_disk->total_sectors * ide_disk->sector_size;
    bdev->supports_lba48 = ide_disk->supports_lba48;
    
    /* Устанавливаем обработчики */
    bdev->read_sectors = ide_read_wrapper;
    bdev->write_sectors = ide_write_wrapper;
    bdev->flush_cache = ide_flush_wrapper;
    
    /* Сохраняем специфичные данные */
    bdev->device_data.ide.ide_disk = ide_disk;
    bdev->device_data.ide.channel = channel;
    bdev->device_data.ide.drive = drive;
    
    /* Устанавливаем статус */
    bdev->status = BLOCKDEV_READY;
    
    term_printf(term, "[BLOCKDEV] IDE device %s registered (%lu MB)\n", /* %llu -> %lu */
               name, (unsigned long)(bdev->total_bytes / (1024 * 1024))); /* Приведение типа */
}

void blockdev_register_ahci(void *ahci_port_ptr, void *ahci_ctrl_ptr,
                           const char *name, uint8_t port_num) {
    ahci_port_t *port = (ahci_port_t*)ahci_port_ptr;
    ahci_controller_t *ctrl = (ahci_controller_t*)ahci_ctrl_ptr;
    
    if (!port || !ctrl || !port->has_device) {
        term_printf(term, "[BLOCKDEV] AHCI port %d has no device\n", port_num);
        return;
    }
    
    /* Создаём блочное устройство */
    blockdev_t *bdev = blockdev_register(name, BLOCKDEV_TYPE_AHCI);
    if (!bdev) return;
    
    /* Заполняем информацию */
    bdev->sector_size = port->sector_size;
    bdev->total_sectors = port->sectors;
    bdev->total_bytes = port->sectors * port->sector_size;
    bdev->supports_lba48 = port->lba48;
    
    /* Устанавливаем обработчики AHCI */
    bdev->read_sectors = ahci_read_wrapper;
    bdev->write_sectors = ahci_write_wrapper;
    bdev->flush_cache = ahci_flush_wrapper;
    
    /* Сохраняем специфичные данные */
    bdev->device_data.ahci.ahci_port = port;
    bdev->device_data.ahci.ahci_ctrl = ctrl;
    bdev->device_data.ahci.port_num = port_num;
    
    /* Устанавливаем статус */
    bdev->status = BLOCKDEV_READY;
    
    term_printf(term, "[BLOCKDEV] AHCI device %s registered (%lu MB)\n",
               name, (unsigned long)(bdev->total_bytes / (1024 * 1024)));
}

/* Автоматическое сканирование и регистрация всех дисков */
void blockdev_scan_all_disks(void) {
    term_printf(term, "[BLOCKDEV] Scanning for all disks...\n");
    
    int disk_counter = 1;
    char dev_name[BLOCKDEV_NAME_LEN];
    
    /* 1. Сканируем IDE диски */
    term_printf(term, "  Scanning IDE controllers...\n");
    for (int ch = 0; ch < 2; ch++) {
        for (int dr = 0; dr < 2; dr++) {
            int idx = ch * 2 + dr;
            if (disks[idx].type != IDE_TYPE_NONE) {
                snprintf(dev_name, sizeof(dev_name), "dsk_%d", disk_counter++);
                blockdev_register_ide(&disks[idx], dev_name, ch, dr);
            }
        }
    }
    
    /* 2. Сканируем AHCI диски */
    term_printf(term, "  Scanning AHCI controller...\n");
    ahci_controller_t *ctrl = ahci_get_controller();
    
    if (ctrl && ctrl->initialized) {
        for (uint8_t port_num = 0; port_num < ctrl->port_count; port_num++) {
            ahci_port_t *port = &ctrl->ports[port_num];
            
            if (port->active && port->has_device) {
                snprintf(dev_name, sizeof(dev_name), "dsk_%d", disk_counter++);
                blockdev_register_ahci(port, ctrl, dev_name, port_num);
            }
        }
    }
    
    term_printf(term, "[BLOCKDEV] Scan complete. Found %d devices.\n", 
                blockdev_count);
}