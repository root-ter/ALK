// fs/devfs/devfs.h
#ifndef DEVFS_H
#define DEVFS_H

#include <stdint.h>
#include "../vfs/vfs.h"
#include "../../base/term/tio.h"

// Структура драйвера устройства
typedef struct device_driver {
    char name[32];
    
    // Для символьных устройств (stdin/stdout)
    int (*read)(void *buf, size_t count, size_t *read);
    int (*write)(const void *buf, size_t count, size_t *written);
    int (*ioctl)(uint32_t cmd, void *arg);
    
    // Для блочных устройств - исправленные сигнатуры
    int (*read_blocks)(void *priv, uint64_t lba, uint32_t count, void *buf);
    int (*write_blocks)(void *priv, uint64_t lba, uint32_t count, const void *buf);
    
    struct device_driver *next;
} device_driver_t;

// Структура для представления устройства в VFS
typedef struct devfs_node {
    char name[32];
    int type;  // FT_CHRDEV или FT_BLKDEV
    device_driver_t *driver;
    void *private;  // Для блочных устройств - указатель на blockdev_t
    struct devfs_node *next;
} devfs_node_t;

// Инициализация /dev
void devfs_init(void);

vfs_inode_t *devfs_create_dir(const char *name);

// Регистрация драйвера устройства
int devfs_register_driver(device_driver_t *drv);

// Создать узел устройства
int devfs_mknod(const char *name, int type, device_driver_t *drv, void *private);

int devfs_mknod_in(vfs_inode_t *dir, const char *name, int type, device_driver_t *drv, void *private);

// Получить инод устройства
vfs_inode_t *devfs_get_inode(const char *path);

void devfs_init_std(vfs_inode_t *dir);
void devfs_init_blk(vfs_inode_t *dir);

// Глобальная переменная для корня devfs (нужна для сравнения в std.c)
extern vfs_inode_t *devfs_root;

#endif