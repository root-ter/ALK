#include "vfs.h"
#include "../../base/mem/mem.h"
#include "../../libc/string.h"
#include "../../base/term/tio.h"
#include "../../base/term/cmd.h"

typedef struct mount_point {
    char path[PATH_MAX];
    vfs_inode_t *inode;
    struct mount_point *next;
} mount_point_t;

// Глобальные переменные
static file_system_t *fs_list = NULL;
static vfs_inode_t *root_inode = NULL;
static mount_point_t *mounts = NULL;

// Простейший аллокатор инодов (в реальности нужно кэширование)
static uint64_t next_ino = 1;

void vfs_init(void) {
    fs_list = NULL;
    root_inode = NULL;
    next_ino = 1;
    tio_printf("[VFS] Initialized\n");
}

int vfs_mount_point(const char *path, vfs_inode_t *inode) {
    mount_point_t *mp = (mount_point_t*)malloc(sizeof(mount_point_t));
    if (!mp) return -1;  // <-- ЭТО ВАЖНО!
    
    strcpy(mp->path, path);
    mp->inode = inode;
    mp->next = mounts;
    mounts = mp;
    return 0;
}

int build_current_path(vfs_inode_t *dir, const char *component, char *out) {
    if (!dir || !out) return -1;
    
    char base_path[PATH_MAX];
    
    // Строим путь до текущей директории
    if (build_path_recursive(dir, base_path, 0) != 0) {
        return -1;
    }
    
    // Если компонент не указан, возвращаем путь к директории
    if (!component || component[0] == '\0') {
        strcpy(out, base_path);
        return 0;
    }
    
    // Добавляем компонент к пути с проверкой длины
    int base_len = strlen(base_path);
    int comp_len = strlen(component);
    
    // Проверяем, что не выйдем за пределы
    if (base_len + comp_len + 2 > PATH_MAX) {
        return -1;  // Путь слишком длинный
    }
    
    if (base_len > 0 && base_path[base_len-1] == '/') {
        snprintf(out, PATH_MAX, "%s%s", base_path, component);
    } else {
        snprintf(out, PATH_MAX, "%s/%s", base_path, component);
    }
    
    return 0;
}

static int get_parent_inode(vfs_inode_t *inode, vfs_inode_t **parent) {
    if (!inode || !parent) return -1;
    
    // Если это корень
    if (inode == root_inode) {
        *parent = root_inode;
        return 0;
    }
    
    // Используем операцию parent из ФС
    if (inode->i_op && inode->i_op->parent) {
        return inode->i_op->parent(inode, parent);
    }
    
    return -1;
}

static vfs_inode_t *check_mount_points(const char *path) {
    tio_printf("[VFS] Checking mount points for '%s'\n", path);
    
    mount_point_t *mp = mounts;
    int found = 0;
    
    while (mp) {
        tio_printf("[VFS] Mount point: '%s' -> inode %p\n", mp->path, mp->inode);
        if (strcmp(mp->path, path) == 0) {
            tio_printf("[VFS] Found match!\n");
            return mp->inode;
        }
        mp = mp->next;
    }
    
    tio_printf("[VFS] No mount point found for '%s'\n", path);
    return NULL;
}

int vfs_register_fs(file_system_t *fs) {
    if (!fs) return -1;
    
    fs->next = fs_list;
    fs_list = fs;
    tio_printf("[VFS] Registered filesystem: %s\n", fs->name);
    return 0;
}

int vfs_mount(const char *fs_name, blockdev_t *dev, vfs_inode_t **root) {
    if (!fs_name || !dev || !root) return -1;
    
    file_system_t *fs = fs_list;
    while (fs) {
        if (strcmp(fs->name, fs_name) == 0) {
            int ret = fs->mount(dev, root);
            if (ret == 0) {
                root_inode = *root;
                tio_printf("[VFS] Mounted %s\n", fs_name);
            }
            return ret;
        }
        fs = fs->next;
    }
    
    tio_printf("[VFS] Filesystem %s not found\n", fs_name);
    return -1;
}

int vfs_unmount(vfs_inode_t *root) {
    if (!root) return -1;
    // TODO: sync all files, free resources
    root_inode = NULL;
    return 0;
}

// Разбор пути на компоненты
static int split_path(const char *path, char *name, const char **rest) {
    if (!path || path[0] == '\0') return -1;
    
    // Пропускаем ведущие слеши
    while (*path == '/') path++;
    if (*path == '\0') return -1;
    
    const char *end = path;
    while (*end && *end != '/') end++;
    
    int len = end - path;
    if (len >= NAME_MAX) len = NAME_MAX - 1;
    
    memcpy(name, path, len);
    name[len] = '\0';
    
    *rest = *end ? end : NULL;
    return 0;
}

// Обход пути с поддержкой точек монтирования
int vfs_walk(vfs_inode_t *dir, const char *path, vfs_inode_t **result) {
    if (!dir || !path || !result) return -1;
    
    // Пустой путь или корень
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        // Проверяем, не является ли текущая директория точкой монтирования
        vfs_inode_t *mount_point = check_mount_points("/");
        if (mount_point) {
            *result = mount_point;
        } else {
            *result = dir;
        }
        return 0;
    }
    
    // Абсолютный путь начинается с корня
    if (path[0] == '/') {
    	if (!root_inode) return -1;
    
    	// Проверяем, не является ли путь точкой монтирования
    	vfs_inode_t *mount_point = check_mount_points(path);
    	if (mount_point) {
            *result = mount_point;
            return 0;
    	}
    
    	// Проверяем, не является ли первый компонент точкой монтирования
    	char first_component[256];
    	const char *rest;
    	if (split_path(path + 1, first_component, &rest) == 0) {
            char mount_path[PATH_MAX];
            snprintf(mount_path, sizeof(mount_path), "/%s", first_component);
        
            mount_point = check_mount_points(mount_path);
            if (mount_point) {
            	if (!rest) {
                    *result = mount_point;
                    return 0;
            	}
                return vfs_walk(mount_point, rest, result);
            }
    	}
    
        return vfs_walk(root_inode, path + 1, result);
    }
    
    // Относительный путь
    char component[NAME_MAX];
    const char *rest;
    
    if (split_path(path, component, &rest) != 0) return -1;
    
    // Проверяем текущую директорию
    if (strcmp(component, ".") == 0) {
        if (!rest) {
            *result = dir;
            return 0;
        }
        return vfs_walk(dir, rest, result);
    }
    
    // Поднимаемся наверх
    if (strcmp(component, "..") == 0) {
        vfs_inode_t *parent;
        if (get_parent_inode(dir, &parent) != 0) {
            return -1;
        }
        
        if (!rest) {
            *result = parent;
            return 0;
        }
        return vfs_walk(parent, rest, result);
    }
    
    // Ищем компонент в текущей директории
    vfs_inode_t *next;
    if (!dir->i_op || !dir->i_op->lookup) return -1;
    
    if (dir->i_op->lookup(dir, component, &next) != 0) {
        return -1;  // Not found
    }
    
    // Формируем текущий путь для проверки точки монтирования
    char current_path[PATH_MAX];
    if (build_current_path(dir, component, current_path) == 0) {
        // Проверяем, не является ли этот путь точкой монтирования
        vfs_inode_t *mount_point = check_mount_points(current_path);
        if (mount_point) {
            // Если это точка монтирования, подменяем инод
            vfs_free_inode(next);
            next = mount_point;
        }
    }
    
    if (!rest) {
        *result = next;
        return 0;
    }
    
    // Должна быть директория для продолжения пути
    if (next->i_mode != FT_DIR) {
        vfs_free_inode(next);
        return -1;
    }
    
    return vfs_walk(next, rest, result);
}

// Обход с получением родительской директории
int vfs_walk_parent(vfs_inode_t *dir, const char *path, 
                    vfs_inode_t **parent, char *name) {
    if (!dir || !path || !parent || !name) return -1;
    
    // Абсолютный путь
    if (path[0] == '/') {
        if (!root_inode) return -1;
        return vfs_walk_parent(root_inode, path + 1, parent, name);
    }
    
    char component[NAME_MAX];
    const char *rest;
    
    if (split_path(path, component, &rest) != 0) return -1;
    
    if (!rest) {
        // Это последний компонент
        *parent = dir;
        strcpy(name, component);
        return 0;
    }
    
    // Ищем промежуточную директорию
    vfs_inode_t *next;
    if (dir->i_op->lookup(dir, component, &next) != 0) return -1;
    
    if (next->i_mode != FT_DIR) return -1;
    
    return vfs_walk_parent(next, rest, parent, name);
}

int vfs_lookup(vfs_inode_t *dir, const char *name, vfs_inode_t **result) {
    if (!dir || !name || !result) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    if (!dir->i_op || !dir->i_op->lookup) return -1;
    
    return dir->i_op->lookup(dir, name, result);
}

int vfs_open(vfs_inode_t *dir, const char *path, uint32_t flags, vfs_file_t **file) {
    if (!dir || !path || !file) return -1;
    
    vfs_inode_t *inode = NULL;
    vfs_inode_t *parent = NULL;
    char name[NAME_MAX];
    
    // Пытаемся найти файл
    if (vfs_walk(dir, path, &inode) == 0) {
        // Файл существует
        if (flags & O_CREAT) {
            // O_CREAT с существующим файлом - просто открываем
        }
    } else {
        // Файл не найден
        if (!(flags & O_CREAT)) return -1;
        
        // Создаем новый файл
        if (vfs_walk_parent(dir, path, &parent, name) != 0) return -1;
        if (!parent->i_op || !parent->i_op->create) return -1;
        
        if (parent->i_op->create(parent, name, FT_REG_FILE, &inode) != 0) {
            return -1;
        }
    }
    
    // Создаем файловый дескриптор
    vfs_file_t *f = (vfs_file_t*)malloc(sizeof(vfs_file_t));
    if (!f) return -1;
    
    f->f_inode = inode;
    f->f_pos = (flags & O_APPEND) ? inode->i_size : 0;
    f->f_flags = flags;
    f->f_private = NULL;
    
    *file = f;
    return 0;
}

int vfs_close(vfs_file_t *file) {
    if (!file) return -1;
    
    if (file->f_inode && file->f_inode->i_dirty) {
        vfs_sync(file->f_inode);
    }
    
    free(file);
    return 0;
}

int vfs_read(vfs_file_t *file, void *buf, uint32_t size, uint32_t *read) {
    if (!file || !file->f_inode || !file->f_inode->i_fop || 
        !file->f_inode->i_fop->read) return -1;
    
    int ret = file->f_inode->i_fop->read(file->f_inode, file->f_pos, 
                                          buf, size, read);
    if (ret == 0) {
        file->f_pos += *read;
    }
    return ret;
}

int vfs_write(vfs_file_t *file, const void *buf, uint32_t size, uint32_t *written) {
    if (!file || !file->f_inode || !file->f_inode->i_fop || 
        !file->f_inode->i_fop->write) return -1;
    
    int ret = file->f_inode->i_fop->write(file->f_inode, file->f_pos, 
                                           buf, size, written);
    if (ret == 0) {
        file->f_pos += *written;
        if (file->f_pos > file->f_inode->i_size) {
            file->f_inode->i_size = file->f_pos;
        }
        file->f_inode->i_dirty = 1;
    }
    return ret;
}

int vfs_seek(vfs_file_t *file, uint64_t offset, int whence) {
    if (!file || !file->f_inode) return -1;
    
    switch (whence) {
        case 0: // SEEK_SET
            file->f_pos = offset;
            break;
        case 1: // SEEK_CUR
            file->f_pos += offset;
            break;
        case 2: // SEEK_END
            file->f_pos = file->f_inode->i_size + offset;
            break;
        default:
            return -1;
    }
    
    return 0;
}

int vfs_mkdir(vfs_inode_t *dir, const char *name, uint32_t mode, vfs_inode_t **result) {
    if (!dir || !name) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    if (!dir->i_op || !dir->i_op->mkdir) return -1;
    
    return dir->i_op->mkdir(dir, name, mode, result);
}

int vfs_rmdir(vfs_inode_t *dir, const char *name) {
    if (!dir || !name) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    if (!dir->i_op || !dir->i_op->rmdir) return -1;
    
    return dir->i_op->rmdir(dir, name);
}

int vfs_readdir(vfs_inode_t *dir, uint64_t *pos, char *name, 
                uint32_t *name_len, uint32_t *type) {
    if (!dir || !pos || !name || !name_len || !type) {
        return -1;
    }
    
    // Проверяем, что это директория
    if (dir->i_mode != FT_DIR) {
        return -1;
    }
    
    // Особый случай: корневая директория VFS
    if (dir == root_inode) {
        // Сначала показываем точки монтирования
        static mount_entry_t mount_entries[32];
        static int mount_count = -1;
        static int mount_index = 0;
        
        if (*pos == 0) {
            // Обновляем список точек монтирования
            mount_count = vfs_get_mount_points("/", mount_entries, 32);
            mount_index = 0;
        }
        
        // Показываем точки монтирования
        while (mount_index < mount_count) {
            if (mount_index == *pos) {
                strcpy(name, mount_entries[mount_index].name);
                *name_len = strlen(name);
                *type = mount_entries[mount_index].type;
                (*pos)++;
                mount_index++;
                return 0;
            }
            mount_index++;
        }
        
        // После точек монтирования показываем содержимое реальной ФС
        if (dir->i_op && dir->i_op->readdir) {
            // Корректируем позицию для ФС
            uint64_t fs_pos = *pos - mount_count;
            int ret = dir->i_op->readdir(dir, &fs_pos, name, name_len, type);
            if (ret == 0) {
                *pos = fs_pos + mount_count;
            }
            return ret;
        }
        
        return -1;
    }
    
    // Для остальных директорий - обычное поведение
    if (!dir->i_op || !dir->i_op->readdir) {
        return -1;
    }
    
    return dir->i_op->readdir(dir, pos, name, name_len, type);
}

int vfs_create(vfs_inode_t *dir, const char *name, uint32_t mode, vfs_inode_t **result) {
    if (!dir || !name) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    if (!dir->i_op || !dir->i_op->create) return -1;
    
    return dir->i_op->create(dir, name, mode, result);
}

int vfs_unlink(vfs_inode_t *dir, const char *name) {
    if (!dir || !name) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    if (!dir->i_op || !dir->i_op->unlink) return -1;
    
    return dir->i_op->unlink(dir, name);
}

int vfs_rename(vfs_inode_t *old_dir, const char *old_name,
               vfs_inode_t *new_dir, const char *new_name) {
    if (!old_dir || !old_name || !new_dir || !new_name) return -1;
    if (!old_dir->i_op || !old_dir->i_op->rename) return -1;
    
    return old_dir->i_op->rename(old_dir, old_name, new_dir, new_name);
}

int vfs_stat(vfs_inode_t *inode, vfs_stat_t *stat) {
    if (!inode || !stat) return -1;
    
    stat->st_mode = inode->i_mode;
    stat->st_uid = inode->i_uid;
    stat->st_gid = inode->i_gid;
    stat->st_size = inode->i_size;
    stat->st_ctime = inode->i_ctime;
    stat->st_mtime = inode->i_mtime;
    stat->st_atime = inode->i_atime;
    stat->st_ino = inode->i_ino;
    stat->st_nlink = inode->i_nlink;
    
    return 0;
}

int vfs_chmod(vfs_inode_t *inode, uint32_t mode) {
    if (!inode) return -1;
    if (!inode->i_op || !inode->i_op->chmod) return -1;
    
    return inode->i_op->chmod(inode, mode);
}

int vfs_sync(vfs_inode_t *inode) {
    if (!inode) return -1;
    if (!inode->i_fop || !inode->i_fop->sync) return -1;
    
    int ret = inode->i_fop->sync(inode);
    if (ret == 0) inode->i_dirty = 0;
    return ret;
}

vfs_inode_t *vfs_alloc_inode(void) {
    vfs_inode_t *inode = (vfs_inode_t*)malloc(sizeof(vfs_inode_t));
    if (!inode) return NULL;
    
    memset(inode, 0, sizeof(vfs_inode_t));
    inode->i_ino = next_ino++;
    return inode;
}

void vfs_free_inode(vfs_inode_t *inode) {
    if (inode) free(inode);
}

int vfs_parent(vfs_inode_t *inode, vfs_inode_t **parent) {
    if (!inode || !parent) return -1;
    
    // Если это корень
    if (inode == root_inode) {
        *parent = root_inode;
        return 0;
    }
    
    // Вызываем специфичную для ФС реализацию
    if (inode->i_op && inode->i_op->parent) {
        return inode->i_op->parent(inode, parent);
    }
    
    return -1;
}

int vfs_get_mount_points(const char *path, mount_entry_t *entries, int max_entries) {
    if (!path || !entries || max_entries <= 0) return -1;
    
    int count = 0;
    mount_point_t *mp = mounts;
    size_t path_len = strlen(path);
    
    while (mp && count < max_entries) {
        // Проверяем, находится ли точка монтирования непосредственно в этом пути
        if (strncmp(mp->path, path, path_len) == 0) {
            // Пропускаем сам путь
            if (strcmp(mp->path, path) == 0) {
                mp = mp->next;
                continue;
            }
            
            // Получаем следующий компонент после path
            const char *rest = mp->path + path_len;
            if (*rest == '/') rest++;
            
            // Находим первый компонент имени
            const char *end = rest;
            while (*end && *end != '/') end++;
            
            int name_len = end - rest;
            if (name_len > 0 && name_len < 256) {
                strncpy(entries[count].name, rest, name_len);
                entries[count].name[name_len] = '\0';
                entries[count].inode = mp->inode;
                entries[count].type = FT_DIR;  // Точки монтирования - всегда директории
                count++;
            }
        }
        mp = mp->next;
    }
    
    return count;
}