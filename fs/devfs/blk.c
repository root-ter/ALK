// fs/devfs/blk.c
#include "devfs.h"
#include "../../drv/block/blockdev.h"
#include "../../base/mem/mem.h"
#include "../../base/term/tio.h"
#include "../../libc/string.h"

// Обертки для блочных устройств
static int blk_read_blocks(void *priv, uint64_t lba, uint32_t count, void *buf) {
    blockdev_t *dev = (blockdev_t*)priv;
    return blockdev_read(dev, lba, count, buf);
}

static int blk_write_blocks(void *priv, uint64_t lba, uint32_t count, const void *buf) {
    blockdev_t *dev = (blockdev_t*)priv;
    return blockdev_write(dev, lba, count, buf);
}

// Шаблон драйвера
static device_driver_t blk_driver_template = {
    .name = "block",
    .read = NULL,
    .write = NULL,
    .ioctl = NULL,
    .read_blocks = blk_read_blocks,
    .write_blocks = blk_write_blocks
};

// Инициализация блочных устройств в указанной директории
void devfs_init_blk(vfs_inode_t *dir) {
    blockdev_t *devices[16];
    int count = blockdev_get_list(devices, 16);
    
    for (int i = 0; i < count; i++) {
        // Создаем копию драйвера
        device_driver_t *drv = (device_driver_t*)malloc(sizeof(device_driver_t));
        memcpy(drv, &blk_driver_template, sizeof(device_driver_t));
        
        // Создаем узел в указанной директории
        char node_name[32];
        strncpy(node_name, devices[i]->name, 31);
        node_name[31] = '\0';
        
        devfs_mknod_in(dir, node_name, FT_BLKDEV, drv, devices[i]);
        
        tio_printf("[DEVFS] Registered blk/%s\n", node_name);
    }
}