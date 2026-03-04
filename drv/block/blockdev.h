#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Максимальное количество дисков в системе */
#define MAX_BLOCK_DEVS 16
#define BLOCKDEV_NAME_LEN 32

/* Типы блочных устройств */
typedef enum {
    BLOCKDEV_TYPE_NONE = 0,
    BLOCKDEV_TYPE_IDE,
    BLOCKDEV_TYPE_AHCI,
    BLOCKDEV_TYPE_RAMDISK
} blockdev_type_t;

/* Статус устройства */
typedef enum {
    BLOCKDEV_UNINITIALIZED = 0,
    BLOCKDEV_READY,
    BLOCKDEV_ERROR,
    BLOCKDEV_NO_MEDIA
} blockdev_status_t;

/* Основная структура блочного устройства */
typedef struct blockdev {
    /* Идентификация */
    char name[BLOCKDEV_NAME_LEN];  /* dsk_1, dsk_2 и т.д. */
    blockdev_type_t type;
    blockdev_status_t status;
    
    /* Геометрия */
    uint64_t sector_size;      /* Размер сектора в байтах (обычно 512) */
    uint64_t total_sectors;    /* Общее количество секторов */
    uint64_t total_bytes;      /* Общий размер в байтах */
    bool supports_lba48;       /* Поддержка LBA48 */
    
    /* Функции-обработчики (аналогично VFS) */
    int (*read_sectors)(struct blockdev *dev, uint64_t lba, 
                        uint32_t count, void *buffer);
    int (*write_sectors)(struct blockdev *dev, uint64_t lba, 
                         uint32_t count, const void *buffer);
    int (*flush_cache)(struct blockdev *dev);
    
    /* Статистика */
    uint64_t read_count;
    uint64_t write_count;
    uint64_t error_count;
    
    /* Специфичные данные для разных типов устройств */
    union {
        struct {
            void *ide_disk;    /* Указатель на ide_disk_t */
            uint8_t channel;   /* IDE_CHANNEL_PRIMARY/SECONDARY */
            uint8_t drive;     /* 0=master, 1=slave */
        } ide;

    struct {
        void *ahci_port;    // <-- указатель на ahci_port_t
        int   port_num;     // <-- номер порта
    } ahci;
        
        struct {
            void *ramdisk_data;
            size_t ramdisk_size;
        } ramdisk;
        
    } device_data;
    
    /* Ссылка для списка */
    struct blockdev *next;
} blockdev_t;

/* ==================== API ==================== */

/* Инициализация системы блочных устройств */
void blockdev_init(void);

/* Регистрация нового устройства */
blockdev_t* blockdev_register(const char *name, blockdev_type_t type);

/* Специальные функции регистрации для конкретных типов устройств */
void blockdev_register_ide(void *ide_disk, const char *name, 
                          uint8_t channel, uint8_t drive);
void blockdev_scan_all_disks(int type);

/* Поиск устройства по имени */
blockdev_t* blockdev_find(const char *name);

/* Поиск устройства по номеру */
blockdev_t* blockdev_find_by_number(int num);

/* Получение списка всех устройств */
int blockdev_get_list(blockdev_t **list, int max_count);

/* Чтение секторов (универсальная функция) */
int blockdev_read(blockdev_t *dev, uint64_t lba, 
                  uint32_t count, void *buffer);

/* Запись секторов (универсальная функция) */
int blockdev_write(blockdev_t *dev, uint64_t lba, 
                   uint32_t count, const void *buffer);

/* Сброс кэша устройства */
int blockdev_flush(blockdev_t *dev);

/* Получение информации об устройстве */
void blockdev_get_info(blockdev_t *dev, char *buffer, size_t buf_size);

/* Удаление устройства (при извлечении) */
void blockdev_unregister(blockdev_t *dev);

/* Отладочный вывод всех устройств */
void blockdev_dump_all(void);

/* Утилиты для работы с дисками */
static inline uint64_t blockdev_get_size(blockdev_t *dev) {
    return dev ? dev->total_bytes : 0;
}

static inline uint64_t blockdev_get_sector_size(blockdev_t *dev) {
    return dev ? dev->sector_size : 0;
}

static inline const char* blockdev_get_name(blockdev_t *dev) {
    return dev ? dev->name : "unknown";
}

#endif
