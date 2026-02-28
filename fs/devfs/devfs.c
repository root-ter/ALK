// fs/devfs/devfs.c
#include "devfs.h"
#include "../../base/mem/mem.h"
#include "../../libc/string.h"
#include "../../base/term/tio.h"

// Корневой инод для /dev
vfs_inode_t *devfs_root = NULL;
static device_driver_t *driver_list = NULL;

// Структура для хранения содержимого директории
typedef struct devfs_dir {
    char name[32];
    int type;
    device_driver_t *driver;
    void *private;
    struct devfs_dir *next;  // Следующий элемент в этой директории
} devfs_dir_t;

// Для каждой директории свой список
typedef struct {
    devfs_dir_t *first;
    devfs_dir_t *last;
} devfs_dir_list_t;

// Операции VFS для устройств
static int devfs_lookup(vfs_inode_t *dir, const char *name, vfs_inode_t **result);
static int devfs_readdir(vfs_inode_t *dir, uint64_t *pos, char *name, uint32_t *name_len, uint32_t *type);
static int devfs_read(vfs_inode_t *inode, uint64_t offset, void *buf, uint32_t size, uint32_t *read);
static int devfs_write(vfs_inode_t *inode, uint64_t offset, const void *buf, uint32_t size, uint32_t *written);


static int devfs_get_name(vfs_inode_t *inode, char *name, int max_len) {
    devfs_dir_t *entry = (devfs_dir_t*)inode->i_private;
    if (!entry) return -1;
    
    strncpy(name, entry->name, max_len - 1);
    name[max_len - 1] = '\0';
    return 0;
}

// Операции VFS
static vfs_operations_t devfs_i_op = {
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
    .get_name = devfs_get_name,
    .mkdir = NULL,
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

// Получить список для директории из приватных данных
static devfs_dir_list_t *get_dir_list(vfs_inode_t *dir) {
    if (!dir) return NULL;
    return (devfs_dir_list_t*)dir->i_private;
}

// Создание директории
vfs_inode_t *devfs_create_dir(const char *name) {
    vfs_inode_t *dir = vfs_alloc_inode();
    if (!dir) return NULL;
    
    dir->i_mode = FT_DIR;
    dir->i_op = &devfs_i_op;
    dir->i_fop = &devfs_f_op;
    
    // Создаем список для новой директории
    devfs_dir_list_t *list = (devfs_dir_list_t*)malloc(sizeof(devfs_dir_list_t));
    list->first = NULL;
    list->last = NULL;
    dir->i_private = list;
    
    return dir;
}

// Добавить элемент в директорию
static void devfs_add_to_dir(vfs_inode_t *dir, const char *name, int type, 
                             device_driver_t *drv, void *private) {
    devfs_dir_list_t *list = get_dir_list(dir);
    if (!list) return;
    
    devfs_dir_t *entry = (devfs_dir_t*)malloc(sizeof(devfs_dir_t));
    strncpy(entry->name, name, 31);
    entry->name[31] = '\0';
    entry->type = type;
    entry->driver = drv;
    entry->private = private;
    entry->next = NULL;
    
    if (list->last) {
        list->last->next = entry;
        list->last = entry;
    } else {
        list->first = entry;
        list->last = entry;
    }
}

// Регистрация драйвера
int devfs_register_driver(device_driver_t *drv) {
    drv->next = driver_list;
    driver_list = drv;
    return 0;
}

// Создание узла устройства в указанной директории
int devfs_mknod_in(vfs_inode_t *dir, const char *name, int type, 
                   device_driver_t *drv, void *private) {
    if (!dir || !name) return -1;
    
    devfs_add_to_dir(dir, name, type, drv, private);
    return 0;
}

// Операция lookup
static int devfs_lookup(vfs_inode_t *dir, const char *name, vfs_inode_t **result) {
    if (!dir) {
        tio_printf("[DEVFS] ERROR: dir is NULL\n");
        return -1;
    }

    devfs_dir_list_t *list = get_dir_list(dir);
    if (!list) return -1;
    
    // Ищем в текущей директории
    devfs_dir_t *entry = list->first;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            vfs_inode_t *inode = vfs_alloc_inode();
            inode->i_mode = entry->type;
            inode->i_size = 0;
            inode->i_private = entry;  // Храним entry в приватных данных
            inode->i_op = &devfs_i_op;
            inode->i_fop = &devfs_f_op;
            
            *result = inode;
            return 0;
        }
        entry = entry->next;
    }
    
    return -1;
}

// Операция readdir
static int devfs_readdir(vfs_inode_t *dir, uint64_t *pos, char *name, 
                         uint32_t *name_len, uint32_t *type) {
    devfs_dir_list_t *list = get_dir_list(dir);
    if (!list) return -1;
    
    int index = 0;
    devfs_dir_t *entry = list->first;
    
    while (entry) {
        if (index == *pos) {
            strcpy(name, entry->name);
            *name_len = strlen(entry->name);
            *type = entry->type;
            (*pos)++;
            return 0;
        }
        index++;
        entry = entry->next;
    }
    
    return -1;
}

// Чтение из устройства
static int devfs_read(vfs_inode_t *inode, uint64_t offset, void *buf, 
                      uint32_t size, uint32_t *read) {
    devfs_dir_t *entry = (devfs_dir_t*)inode->i_private;
    if (!entry || !entry->driver) return -1;
    
    if (entry->type == FT_CHRDEV) {
        if (!entry->driver->read) return -1;
        return entry->driver->read(buf, size, (size_t*)read);
    } else if (entry->type == FT_BLKDEV) {
        if (!entry->driver->read_blocks) return -1;
        uint64_t lba = offset / 512;
        uint32_t count = (size + 511) / 512;
        int ret = entry->driver->read_blocks(entry->private, lba, count, buf);
        if (ret == 0) *read = size;
        return ret;
    }
    
    return -1;
}

// Запись в устройство
static int devfs_write(vfs_inode_t *inode, uint64_t offset, const void *buf, 
                       uint32_t size, uint32_t *written) {
    devfs_dir_t *entry = (devfs_dir_t*)inode->i_private;
    if (!entry || !entry->driver) return -1;
    
    if (entry->type == FT_CHRDEV) {
        if (!entry->driver->write) return -1;
        return entry->driver->write(buf, size, (size_t*)written);
    } else if (entry->type == FT_BLKDEV) {
        if (!entry->driver->write_blocks) return -1;
        uint64_t lba = offset / 512;
        uint32_t count = (size + 511) / 512;
        int ret = entry->driver->write_blocks(entry->private, lba, count, buf);
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
    tio_printf("[DEVFS] devfs_root = %p\n", devfs_root);
    tio_printf("[DEVFS] devfs_root->i_mode = %d\n", devfs_root->i_mode);
    tio_printf("[DEVFS] devfs_root->i_op = %p\n", devfs_root->i_op);
    tio_printf("[DEVFS] devfs_root->i_fop = %p\n", devfs_root->i_fop);
    tio_printf("[DEVFS] devfs_root->i_private = %p\n", devfs_root->i_private);
    
    // Регистрируем /dev в VFS
    vfs_mount_point("/dev", devfs_root);
    
    // Создаем подпапку /dev/blk
    vfs_inode_t *blk_dir = devfs_create_dir("blk");
    if (blk_dir) {
        // Добавляем blk в /dev
        devfs_add_to_dir(devfs_root, "blk", FT_DIR, NULL, blk_dir);
        devfs_init_blk(blk_dir);
    }
    
    // Создаем подпапку /dev/std
    vfs_inode_t *std_dir = devfs_create_dir("std");
    if (std_dir) {
        // Добавляем std в /dev
        devfs_add_to_dir(devfs_root, "std", FT_DIR, NULL, std_dir);
        devfs_init_std(std_dir);
    }
    
    // Создаем отдельную папку /std в корне
    vfs_inode_t *std_root = devfs_create_dir("std");
    if (std_root) {
        vfs_mount_point("/std", std_root);
        devfs_init_std(std_root);
    }
    
    tio_printf("[DEVFS] /dev and /std initialized\n");
}

