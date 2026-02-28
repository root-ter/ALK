// fs/devfs/devfs.c
#include "devfs.h"
#include "../../base/mem/mem.h"
#include "../../libc/string.h"

// Корневой инод для /dev
vfs_inode_t *devfs_root = NULL;
static device_driver_t *driver_list = NULL;

// Операции VFS для устройств
static int devfs_lookup(vfs_inode_t *dir, const char *name, vfs_inode_t **result);
static int devfs_readdir(vfs_inode_t *dir, uint64_t *pos, char *name, uint32_t *name_len, uint32_t *type);
static int devfs_read(vfs_inode_t *inode, uint64_t offset, void *buf, uint32_t size, uint32_t *read);
static int devfs_write(vfs_inode_t *inode, uint64_t offset, const void *buf, uint32_t size, uint32_t *written);

// Список всех устройств
static devfs_node_t *devices = NULL;

// Операции VFS
static vfs_operations_t devfs_i_op = {
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
    .mkdir = NULL,   // Нельзя создавать папки в /dev
    .rmdir = NULL,
    .unlink = NULL,
    .create = NULL,
    .rename = NULL,
    .chmod = NULL,
    .stat = NULL
};

static vfs_file_operations_t devfs_f_op = {
    .read = devfs_read,
    .write = devfs_write,
    .truncate = NULL,
    .sync = NULL
};

vfs_inode_t *devfs_create_dir(const char *name) {
    vfs_inode_t *dir = vfs_alloc_inode();
    if (!dir) return NULL;
    
    dir->i_mode = FT_DIR;
    dir->i_op = &devfs_i_op;
    dir->i_fop = &devfs_f_op;
    
    // Создаем узел в списке устройств
    devfs_node_t *node = (devfs_node_t*)malloc(sizeof(devfs_node_t));
    strncpy(node->name, name, 31);
    node->name[31] = '\0';
    node->type = FT_DIR;
    node->driver = NULL;
    node->private = NULL;
    node->next = devices;
    devices = node;
    
    return dir;
}

// Регистрация драйвера
int devfs_register_driver(device_driver_t *drv) {
    drv->next = driver_list;
    driver_list = drv;
    return 0;
}

// Создание узла устройства
int devfs_mknod(const char *path, int type, device_driver_t *drv, void *private) {
    // Парсим путь
    const char *name = path;
    while (*name == '/') name++;
    
    devfs_node_t *node = (devfs_node_t*)malloc(sizeof(devfs_node_t));
    strncpy(node->name, name, 31);
    node->type = type;
    node->driver = drv;
    node->private = private;
    node->next = devices;
    devices = node;
    
    return 0;
}

int devfs_mknod_in(vfs_inode_t *dir, const char *name, int type, device_driver_t *drv, void *private) {
    // Создаем узел в списке устройств
    devfs_node_t *node = (devfs_node_t*)malloc(sizeof(devfs_node_t));
    if (!node) return -1;
    
    strncpy(node->name, name, 31);
    node->name[31] = '\0';
    node->type = type;
    node->driver = drv;
    node->private = private;
    node->next = devices;
    devices = node;
    
    return 0;
}

// Поиск устройства по имени
static devfs_node_t *find_device(const char *name) {
    devfs_node_t *node = devices;
    while (node) {
        if (strcmp(node->name, name) == 0) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

// Операция lookup
static int devfs_lookup(vfs_inode_t *dir, const char *name, vfs_inode_t **result) {
    devfs_node_t *node = find_device(name);
    if (!node) return -1;
    
    vfs_inode_t *inode = vfs_alloc_inode();
    inode->i_mode = node->type;
    inode->i_size = 0;
    inode->i_private = node;  // Храним узел в приватных данных
    inode->i_op = &devfs_i_op;
    inode->i_fop = &devfs_f_op;
    
    *result = inode;
    return 0;
}

// Операция readdir (для /dev и подпапок)
static int devfs_readdir(vfs_inode_t *dir, uint64_t *pos, char *name, uint32_t *name_len, uint32_t *type) {
    int index = 0;
    devfs_node_t *node = devices;
    
    while (node) {
        if (index == *pos) {
            strcpy(name, node->name);
            *name_len = strlen(node->name);
            *type = node->type;
            (*pos)++;
            return 0;
        }
        index++;
        node = node->next;
    }
    
    return -1;
}

// Чтение из устройства
static int devfs_read(vfs_inode_t *inode, uint64_t offset, void *buf, uint32_t size, uint32_t *read) {
    devfs_node_t *node = (devfs_node_t*)inode->i_private;
    if (!node || !node->driver) return -1;
    
    if (node->type == FT_CHRDEV) {
        if (!node->driver->read) return -1;
        return node->driver->read(buf, size, (size_t*)read);
    } else if (node->type == FT_BLKDEV) {
        if (!node->driver->read_blocks) return -1;
        uint64_t lba = offset / 512;
        uint32_t count = (size + 511) / 512;
        int ret = node->driver->read_blocks(node->private, lba, count, buf);
        if (ret == 0) *read = size;
        return ret;
    }
    
    return -1;
}

// Запись в устройство
static int devfs_write(vfs_inode_t *inode, uint64_t offset, const void *buf, uint32_t size, uint32_t *written) {
    devfs_node_t *node = (devfs_node_t*)inode->i_private;
    if (!node || !node->driver) return -1;
    
    if (node->type == FT_CHRDEV) {
        if (!node->driver->write) return -1;
        return node->driver->write(buf, size, (size_t*)written);
    } else if (node->type == FT_BLKDEV) {
        if (!node->driver->write_blocks) return -1;
        uint64_t lba = offset / 512;
        uint32_t count = (size + 511) / 512;
        int ret = node->driver->write_blocks(node->private, lba, count, buf);
        if (ret == 0) *written = size;
        return ret;
    }
    
    return -1;
}

// Инициализация всей /dev
void devfs_init(void) {
    tio_printf("[DEVFS] Initializing...\n");
    
    // Создаем корневую папку /dev
    devfs_root = devfs_create_dir("dev");
    if (!devfs_root) {
        tio_printf("[DEVFS] Failed to create /dev\n");
        return;
    }
    
    // Регистрируем /dev в VFS
    vfs_mount_point("/dev", devfs_root);
    
    // Создаем подпапку /dev/blk
    vfs_inode_t *blk_dir = devfs_create_dir("blk");
    if (blk_dir) {
        devfs_init_blk(blk_dir);
    }
    
    // Создаем подпапку /dev/std
    vfs_inode_t *std_dir = devfs_create_dir("std");
    if (std_dir) {
        devfs_init_std(std_dir);
    }
    
    // Создаем отдельную папку /std в корне
    vfs_inode_t *std_root = devfs_create_dir("std");
    if (std_root) {
        vfs_mount_point("/std", std_root);
        devfs_init_std(std_root);  // Заполняем теми же устройствами
    }
    
    tio_printf("[DEVFS] /dev and /std initialized\n");
}