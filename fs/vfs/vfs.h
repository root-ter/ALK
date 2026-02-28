#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include "../../drv/block/blockdev.h"

// Опережающие объявления (forward declarations)
struct vfs_inode;
struct vfs_file;

// Типы файлов
#define FT_UNKNOWN   0
#define FT_REG_FILE  1
#define FT_DIR       2
#define FT_CHRDEV    3
#define FT_BLKDEV    4
#define FT_FIFO      5
#define FT_SOCK      6
#define FT_SYMLINK   7

// Флаги открытия
#define O_READ       1
#define O_WRITE      2
#define O_CREAT      4
#define O_TRUNC      8
#define O_APPEND     16

// Максимальная длина пути
#define PATH_MAX     4096
#define NAME_MAX     255

// Операции с инодами (метаданные)
typedef struct vfs_operations {
    int (*lookup)(struct vfs_inode *dir, const char *name, struct vfs_inode **result);
    int (*create)(struct vfs_inode *dir, const char *name, uint32_t mode, struct vfs_inode **result);
    int (*unlink)(struct vfs_inode *dir, const char *name);
    int (*mkdir)(struct vfs_inode *dir, const char *name, uint32_t mode, struct vfs_inode **result);
    int (*rmdir)(struct vfs_inode *dir, const char *name);
    int (*rename)(struct vfs_inode *old_dir, const char *old_name, 
                  struct vfs_inode *new_dir, const char *new_name);
    int (*chmod)(struct vfs_inode *inode, uint32_t mode);
    int (*stat)(struct vfs_inode *inode, void *stat_buf);
    int (*readdir)(struct vfs_inode *dir, uint64_t *pos, char *name, 
                   uint32_t *name_len, uint32_t *type);
    int (*parent)(struct vfs_inode *inode, struct vfs_inode **parent);
    int (*get_name)(struct vfs_inode *inode, char *name, int max_len);
} vfs_operations_t;

// Операции с открытыми файлами (данные)
typedef struct vfs_file_operations {
    int (*read)(struct vfs_inode *inode, uint64_t offset, void *buf, uint32_t size, uint32_t *read);
    int (*write)(struct vfs_inode *inode, uint64_t offset, const void *buf, uint32_t size, uint32_t *written);
    int (*truncate)(struct vfs_inode *inode, uint64_t new_size);
    int (*sync)(struct vfs_inode *inode);
} vfs_file_operations_t;

// Инод (представляет файл или директорию)
typedef struct vfs_inode {
    uint32_t i_mode;        // Тип и права
    uint32_t i_uid;
    uint32_t i_gid;
    uint64_t i_size;
    uint64_t i_ctime;       // Время создания
    uint64_t i_mtime;       // Время изменения
    uint64_t i_atime;       // Время доступа
    uint64_t i_ino;         // Уникальный номер
    
    // Ссылки
    uint32_t i_nlink;       // Количество жестких ссылок
    
    // Операции
    vfs_operations_t *i_op;
    vfs_file_operations_t *i_fop;
    
    // Приватные данные конкретной ФС
    void *i_private;
    
    // Для кэширования
    int i_dirty;
    struct vfs_inode *i_next;  // Для списка
} vfs_inode_t;

// Открытый файл (файловый дескриптор)
typedef struct vfs_file {
    vfs_inode_t *f_inode;
    uint64_t f_pos;
    uint32_t f_flags;
    void *f_private;        // Для конкретной ФС
} vfs_file_t;

// Структура для регистрации файловой системы
typedef struct file_system {
    char name[16];
    int (*mount)(blockdev_t *dev, struct vfs_inode **root);
    int (*unmount)(struct vfs_inode *root);
    struct file_system *next;
} file_system_t;

// Статистика файла (для совместимости с POSIX)
typedef struct vfs_stat {
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_ctime;
    uint64_t st_mtime;
    uint64_t st_atime;
    uint64_t st_ino;
    uint32_t st_nlink;
} vfs_stat_t;


typedef struct {
    char name[256];
    vfs_inode_t *inode;
    int type;
} mount_entry_t;

// ==================== API ДЛЯ ЯДРА ====================

// Инициализация VFS
void vfs_init(void);

// Регистрация файловой системы
int vfs_register_fs(file_system_t *fs);

// Монтирование
int vfs_mount(const char *fs_name, blockdev_t *dev, vfs_inode_t **root);
int vfs_unmount(vfs_inode_t *root);

// Операции с путями
int vfs_walk(vfs_inode_t *dir, const char *path, vfs_inode_t **result);
int vfs_walk_parent(vfs_inode_t *dir, const char *path, vfs_inode_t **parent, char *name);
int vfs_lookup(vfs_inode_t *dir, const char *name, vfs_inode_t **result);

// Открытие/закрытие файлов
int vfs_open(vfs_inode_t *dir, const char *path, uint32_t flags, vfs_file_t **file);
int vfs_close(vfs_file_t *file);

// Чтение/запись
int vfs_read(vfs_file_t *file, void *buf, uint32_t size, uint32_t *read);
int vfs_write(vfs_file_t *file, const void *buf, uint32_t size, uint32_t *written);
int vfs_seek(vfs_file_t *file, uint64_t offset, int whence);

// Операции с директориями
int vfs_mkdir(vfs_inode_t *dir, const char *name, uint32_t mode, vfs_inode_t **result);
int vfs_rmdir(vfs_inode_t *dir, const char *name);
int vfs_readdir(vfs_inode_t *dir, uint64_t *pos, char *name, uint32_t *name_len, uint32_t *type);

// Управление файлами
int vfs_create(vfs_inode_t *dir, const char *name, uint32_t mode, vfs_inode_t **result);
int vfs_unlink(vfs_inode_t *dir, const char *name);
int vfs_rename(vfs_inode_t *old_dir, const char *old_name, 
               vfs_inode_t *new_dir, const char *new_name);

// Метаданные
int vfs_stat(vfs_inode_t *inode, vfs_stat_t *stat);
int vfs_chmod(vfs_inode_t *inode, uint32_t mode);
int vfs_sync(vfs_inode_t *inode);

// Управление инодами (для ФС)
vfs_inode_t *vfs_alloc_inode(void);
void vfs_free_inode(vfs_inode_t *inode);

int vfs_parent(vfs_inode_t *inode, vfs_inode_t **parent);

// Создать точку монтирования
int vfs_mount_point(const char *path, vfs_inode_t *inode);

int build_current_path(vfs_inode_t *dir, const char *component, char *out);

int vfs_get_mount_points(const char *path, mount_entry_t *entries, int max_entries);

#endif