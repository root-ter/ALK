#ifndef EXFAT_H
#define EXFAT_H

#include <stdint.h>
#include "../vfs/vfs.h"

// Сигнатура exFAT
#define EXFAT_SIGNATURE     0x7C8E4FD2A5C14F88  // "EXFAT   " в HEX

// Значения FAT
#define EXFAT_FAT_FREE      0x00000000
#define EXFAT_FAT_END       0xFFFFFFFF
#define EXFAT_FAT_BAD       0xFFFFFFF7

// Типы записей в директории
#define EXFAT_ENTRY_FILE        0x85  // Файл
#define EXFAT_ENTRY_DIR         0x85  // Папка (но с атрибутом)
#define EXFAT_ENTRY_VOLUME      0x83  // Метка тома
#define EXFAT_ENTRY_BITMAP      0x81  // Битмап
#define EXFAT_ENTRY_UPCASE      0x82  // Up-case table
#define EXFAT_ENTRY_NAME        0xC1  // Продолжение имени
#define EXFAT_ENTRY_STREAM      0xC0  // Поток данных

// Атрибуты файлов
#define EXFAT_ATTR_READ_ONLY    0x01
#define EXFAT_ATTR_HIDDEN       0x02
#define EXFAT_ATTR_SYSTEM       0x04
#define EXFAT_ATTR_VOLUME_ID    0x08
#define EXFAT_ATTR_DIRECTORY    0x10
#define EXFAT_ATTR_ARCHIVE      0x20

// Главный загрузочный блок (VBR)
typedef struct {
    uint8_t  jump_boot[3];
    uint8_t  fs_name[8];              // "EXFAT   "
    uint8_t  reserved[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;
    uint8_t  sectors_per_cluster_shift;
    uint8_t  number_of_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved2[7];
    uint8_t  boot_code[390];
    uint16_t signature;                // 0xAA55
} __attribute__((packed)) exfat_vbr_t;
// Запись файла/директории
typedef struct {
    uint8_t  type;                // 0x85
    uint8_t  secondary_count;
    uint16_t checksum;
    uint16_t file_attributes;
    uint16_t reserved1;
    uint32_t create_time;
    uint32_t modify_time;
    uint32_t access_time;
    uint8_t  create_time_10ms;
    uint8_t  modify_time_10ms;
    uint8_t  create_tz_offset;
    uint8_t  modify_tz_offset;
    uint8_t  access_tz_offset;
    uint8_t  reserved2[7];
} __attribute__((packed)) exfat_file_entry_t;

// Запись потока данных
typedef struct {
    uint8_t  type;                // 0xC0
    uint8_t  flags;
    uint8_t  reserved1;
    uint8_t  name_length;
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} __attribute__((packed)) exfat_stream_entry_t;

// Запись имени (UTF-16)
typedef struct {
    uint8_t  type;                // 0xC1
    uint8_t  flags;
    uint16_t name[15];            // 15 UTF-16 символов
} __attribute__((packed)) exfat_name_entry_t;

// Битмап свободных кластеров
typedef struct {
    uint8_t  type;                // 0x81
    uint8_t  flags;
    uint8_t  reserved[18];
    uint32_t first_cluster;
    uint64_t data_length;
} __attribute__((packed)) exfat_bitmap_entry_t;

// Контекст exFAT
typedef struct {
    blockdev_t *dev;
    exfat_vbr_t vbr;
    
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    
    uint64_t fat_start;            // Смещение FAT в байтах
    uint64_t cluster_heap_start;   // Смещение данных в байтах
    uint32_t root_cluster;         // Кластер корня
    
    uint32_t *fat_cache;           // Кэш FAT
    uint32_t fat_entries;
} exfat_t;

// Приватные данные инода
typedef struct {
    exfat_t *exfat;
    uint32_t first_cluster;
    uint64_t data_length;
    uint32_t dir_cluster;      // Кластер директории, где находится запись
    uint32_t dir_entry;        // Номер записи
    uint32_t parent_cluster;   // Кластер родительской директории
} exfat_inode_private_t;

// Инициализация exFAT
void exfat_init(void);
int exfat_format(blockdev_t *dev);
int exfat_get_name(vfs_inode_t *inode, char *name, int max_len);

#endif