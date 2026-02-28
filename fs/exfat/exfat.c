#include "exfat.h"
#include "../../base/mem/mem.h"
#include "../../libc/string.h"
#include "../../base/term/tio.h"

// ==================== ВНУТРЕННИЕ ФУНКЦИИ ====================

// Чтение секторов с диска
static int exfat_read_sectors(exfat_t *exfat, uint64_t sector, uint32_t count, void *buf) {
    return blockdev_read(exfat->dev, sector, count, buf);
}

// Чтение кластера
static int exfat_read_cluster(exfat_t *exfat, uint32_t cluster, uint8_t *buffer) {
    if (cluster < 2) return -1;  // Кластеры 0 и 1 зарезервированы
    
    uint64_t sector = exfat->cluster_heap_start / exfat->bytes_per_sector + 
                     (cluster - 2) * exfat->sectors_per_cluster;
    
    return exfat_read_sectors(exfat, sector, exfat->sectors_per_cluster, buffer);
}

// Запись кластера
static int exfat_write_cluster(exfat_t *exfat, uint32_t cluster, const uint8_t *buffer) {
    if (cluster < 2) return -1;
    
    uint64_t sector = exfat->cluster_heap_start / exfat->bytes_per_sector + 
                     (cluster - 2) * exfat->sectors_per_cluster;
    
    return blockdev_write(exfat->dev, sector, exfat->sectors_per_cluster, buffer);
}

// Получить следующий кластер из FAT
static uint32_t exfat_next_cluster(exfat_t *exfat, uint32_t cluster) {
    if (cluster >= exfat->fat_entries) return 0;
    return exfat->fat_cache[cluster];
}

// Преобразование exFAT времени в UNIX timestamp (упрощенно)
static uint64_t exfat_time_to_unix(uint32_t exfat_time) {
    // exFAT time: 2-second increments from 1980
    // UNIX time: seconds from 1970
    uint64_t days = (exfat_time >> 16) & 0xFFFF;  // Days from 1980
    uint64_t seconds = (exfat_time & 0xFFFF) * 2;
    
    // 1980 to 1970 offset: approximately 10 years of days
    return (days * 86400) + seconds + (10 * 365 * 86400);
}

// ==================== ОПЕРАЦИИ VFS ====================

// Поиск в директории
static int exfat_lookup(vfs_inode_t *dir, const char *name, vfs_inode_t **result) {
    if (dir->i_mode != FT_DIR) return -1;
    
    exfat_inode_private_t *priv = (exfat_inode_private_t*)dir->i_private;
    exfat_t *exfat = priv->exfat;
    
    uint32_t cluster = priv->first_cluster;
    uint8_t buf[exfat->bytes_per_cluster];
    
    while (cluster >= 2 && cluster != EXFAT_FAT_END) {
        exfat_read_cluster(exfat, cluster, buf);
        
        uint8_t *ptr = buf;
        uint8_t *end = buf + exfat->bytes_per_cluster;
        
        while (ptr < end) {
            uint8_t type = ptr[0];
            if (type == 0) break;  // Конец директории
            
            if (type == EXFAT_ENTRY_FILE) {
                exfat_file_entry_t *file = (exfat_file_entry_t*)ptr;
                
                // Ищем stream entry
                uint8_t count = file->secondary_count;
                uint8_t *stream_ptr = ptr + sizeof(exfat_file_entry_t);
                exfat_stream_entry_t *stream = NULL;
                
                for (int i = 0; i < count; i++) {
                    if (stream_ptr[0] == EXFAT_ENTRY_STREAM) {
                        stream = (exfat_stream_entry_t*)stream_ptr;
                        break;
                    }
                    stream_ptr += 32;
                }
                
                if (!stream) {
                    ptr += 32 * (count + 1);
                    continue;
                }
                
                // Ищем имя
                char ascii_name[256];
                int name_len = 0;
                uint8_t *name_ptr = stream_ptr + 32;  // После stream
                
                for (int i = 0; i < count - 1 && name_len < 255; i++) {
                    if (name_ptr[0] == EXFAT_ENTRY_NAME) {
                        exfat_name_entry_t *name_entry = (exfat_name_entry_t*)name_ptr;
                        for (int j = 0; j < 15; j++) {
                            uint16_t c = name_entry->name[j];
                            if (c == 0) break;
                            if (c < 128) ascii_name[name_len++] = (char)c;
                        }
                    }
                    name_ptr += 32;
                }
                ascii_name[name_len] = '\0';
                
                // Сравниваем
                if (strcmp(ascii_name, name) == 0) {
                    // Нашли!
                    vfs_inode_t *inode = vfs_alloc_inode();
                    exfat_inode_private_t *new_priv = 
                        (exfat_inode_private_t*)malloc(sizeof(exfat_inode_private_t));
                    
                    new_priv->exfat = exfat;
                    new_priv->first_cluster = stream->first_cluster;
                    new_priv->data_length = stream->data_length;
                    new_priv->dir_cluster = cluster;
                    new_priv->dir_entry = (ptr - buf) / 32;
		    new_priv->parent_cluster = priv->first_cluster;
                    
                    inode->i_mode = (file->file_attributes & EXFAT_ATTR_DIRECTORY) ? 
                                    FT_DIR : FT_REG_FILE;
                    inode->i_size = stream->data_length;
                    inode->i_private = new_priv;
                    
                    // Копируем операции
                    inode->i_op = dir->i_op;
                    inode->i_fop = dir->i_fop;
                    
                    *result = inode;
                    return 0;
                }
            }
            
            // Следующая запись
            uint8_t count = ptr[1] + 1;  // Основная + secondary
            ptr += 32 * count;
        }
        
        cluster = exfat_next_cluster(exfat, cluster);
    }
    
    return -1;
}

static int exfat_mkdir(vfs_inode_t *dir, const char *name, uint32_t mode, vfs_inode_t **new_inode);

// Создание файла
// Создание файла
static int exfat_create(vfs_inode_t *dir, const char *name, uint32_t mode, vfs_inode_t **result) {
    // Создаем файл и получаем инод
    int ret = exfat_mkdir(dir, name, mode, result);
    if (ret != 0) return ret;
    
    return 0;  // Уже вернули инод через result
}

// Удаление файла
static int exfat_unlink(vfs_inode_t *dir, const char *name) {
    if (!dir || !name) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    
    exfat_inode_private_t *dir_priv = (exfat_inode_private_t*)dir->i_private;
    exfat_t *exfat = dir_priv->exfat;
    
    // Ищем файл
    vfs_inode_t *file = NULL;
    if (exfat_lookup(dir, name, &file) != 0) return -1;
    
    exfat_inode_private_t *file_priv = (exfat_inode_private_t*)file->i_private;
    
    // Освобождаем кластеры файла в FAT
    uint32_t cluster = file_priv->first_cluster;
    while (cluster >= 2 && cluster != EXFAT_FAT_END) {
        uint32_t next = exfat_next_cluster(exfat, cluster);
        exfat->fat_cache[cluster] = EXFAT_FAT_FREE;
        cluster = next;
    }
    
    // Помечаем записи в директории как свободные
    uint8_t buf[exfat->bytes_per_cluster];
    exfat_read_cluster(exfat, dir_priv->first_cluster, buf);
    
    // Находим запись файла
    uint8_t *ptr = buf + file_priv->dir_entry * 32;
    uint8_t count = ptr[1] + 1;  // основная + secondary
    
    // Помечаем все записи как свободные (type = 0)
    for (int i = 0; i < count; i++) {
        ptr[i * 32] = 0;
    }
    
    // Записываем обратно
    exfat_write_cluster(exfat, dir_priv->first_cluster, buf);
    
    // Помечаем, что FAT изменилась
    exfat->fat_cache[0] |= 1;  // dirty flag
    
    vfs_free_inode(file);
    return 0;
}

// Создание директории
// Создание директории - теперь возвращает инод
static int exfat_mkdir(vfs_inode_t *dir, const char *name, uint32_t mode, vfs_inode_t **new_inode) {
    if (!dir || !name) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    
    exfat_inode_private_t *dir_priv = (exfat_inode_private_t*)dir->i_private;
    exfat_t *exfat = dir_priv->exfat;
    
    // Проверяем, нет ли уже такого имени
    vfs_inode_t *existing = NULL;
    if (exfat_lookup(dir, name, &existing) == 0) {
        vfs_free_inode(existing);
        return -1;  // уже существует
    }
    
    // Выделяем кластер для новой директории/файла
    uint32_t new_cluster = 0;
    for (uint32_t i = 2; i < exfat->fat_entries; i++) {
        if (exfat->fat_cache[i] == EXFAT_FAT_FREE) {
            new_cluster = i;
            break;
        }
    }
    if (new_cluster == 0) return -1;  // нет места
    
    exfat->fat_cache[new_cluster] = EXFAT_FAT_END;
    
    // Инициализируем новую директорию (пустую)
    uint8_t cluster_buf[exfat->bytes_per_cluster];
    memset(cluster_buf, 0, exfat->bytes_per_cluster);
    exfat_write_cluster(exfat, new_cluster, cluster_buf);
    
    // Ищем свободное место в родительской директории для новой записи
    uint32_t parent_cluster = dir_priv->first_cluster;
    uint8_t parent_buf[exfat->bytes_per_cluster];
    exfat_read_cluster(exfat, parent_cluster, parent_buf);
    
    // Ищем свободную запись
    int free_entry = -1;
    uint8_t *ptr = parent_buf;
    for (int i = 0; i < exfat->bytes_per_cluster / 32; i++) {
        if (ptr[0] == 0) {  // свободная запись
            free_entry = i;
            break;
        }
        ptr += 32;
    }
    if (free_entry == -1) return -1;  // нет места в директории
    
    // Создаем запись файла (основная)
    exfat_file_entry_t *file_entry = (exfat_file_entry_t*)ptr;
    memset(file_entry, 0, sizeof(exfat_file_entry_t));
    file_entry->type = EXFAT_ENTRY_FILE;
    file_entry->secondary_count = 2;  // stream + имя
    if (mode == FT_DIR) {
        file_entry->file_attributes = EXFAT_ATTR_DIRECTORY;
    } else {
        file_entry->file_attributes = 0;  // обычный файл
    }
    tio_printf("[DEBUG] Creating '%s' with mode=%s, attributes=0x%x\n", 
           name, mode == FT_DIR ? "DIR" : "FILE", file_entry->file_attributes);
    file_entry->create_time = 0;
    file_entry->modify_time = 0;
    
    // Создаем stream entry
    exfat_stream_entry_t *stream = (exfat_stream_entry_t*)(ptr + 32);
    memset(stream, 0, sizeof(exfat_stream_entry_t));
    stream->type = EXFAT_ENTRY_STREAM;
    stream->name_length = strlen(name);
    stream->first_cluster = new_cluster;
    stream->data_length = 0;
    
    // Создаем name entry
    exfat_name_entry_t *name_entry = (exfat_name_entry_t*)(ptr + 64);
    memset(name_entry, 0, sizeof(exfat_name_entry_t));
    name_entry->type = EXFAT_ENTRY_NAME;
    
    // Копируем имя (просто ASCII)
    for (int i = 0; i < strlen(name) && i < 15; i++) {
        name_entry->name[i] = name[i];
    }
    
    // Записываем обратно
    exfat_write_cluster(exfat, parent_cluster, parent_buf);
    
    // Помечаем FAT как dirty
    exfat->fat_cache[0] |= 1;
    
    // СОЗДАЕМ ИНОД ДЛЯ НОВОГО ФАЙЛА/ДИРЕКТОРИИ
    if (new_inode) {
        vfs_inode_t *inode = vfs_alloc_inode();
        exfat_inode_private_t *new_priv = 
            (exfat_inode_private_t*)malloc(sizeof(exfat_inode_private_t));
        
        new_priv->exfat = exfat;
        new_priv->first_cluster = new_cluster;
        new_priv->data_length = 0;
        new_priv->dir_cluster = parent_cluster;
        new_priv->dir_entry = free_entry;
        new_priv->parent_cluster = dir_priv->first_cluster;  // ВАЖНО!
        
        inode->i_mode = mode;
        inode->i_size = 0;
        inode->i_private = new_priv;
        inode->i_op = dir->i_op;
        inode->i_fop = dir->i_fop;
        
        *new_inode = inode;
    }
    
    return 0;
}

// Удаление директории
// exfat.c - удаление директории
static int exfat_rmdir(vfs_inode_t *dir, const char *name) {
    if (!dir || !name) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    
    // Ищем директорию
    vfs_inode_t *subdir = NULL;
    if (exfat_lookup(dir, name, &subdir) != 0) return -1;
    
    if (subdir->i_mode != FT_DIR) {
        vfs_free_inode(subdir);
        return -1;  // не директория
    }
    
    // Проверяем, пустая ли директория
    // TODO: проверить что в ней нет файлов (кроме . и ..)
    
    // Используем exfat_unlink (одинаково для файлов и пустых директорий)
    int ret = exfat_unlink(dir, name);
    vfs_free_inode(subdir);
    return ret;
}

// Переименование
static int exfat_rename(vfs_inode_t *old_dir, const char *old_name,
                        vfs_inode_t *new_dir, const char *new_name) {
    if (!old_dir || !old_name || !new_dir || !new_name) return -1;
    
    exfat_inode_private_t *old_priv = (exfat_inode_private_t*)old_dir->i_private;
    exfat_t *exfat = old_priv->exfat;
    
    // Находим файл
    vfs_inode_t *file = NULL;
    if (exfat_lookup(old_dir, old_name, &file) != 0) return -1;
    
    // Читаем кластер старой директории
    uint8_t old_buf[exfat->bytes_per_cluster];
    exfat_read_cluster(exfat, old_priv->first_cluster, old_buf);
    
    // Находим запись файла
    exfat_inode_private_t *file_priv = (exfat_inode_private_t*)file->i_private;
    uint8_t *old_ptr = old_buf + file_priv->dir_entry * 32;
    uint8_t count = old_ptr[1] + 1;
    
    // Временно сохраняем данные файла
    uint8_t file_data[count * 32];
    memcpy(file_data, old_ptr, count * 32);
    
    // Помечаем старую запись как свободную
    for (int i = 0; i < count; i++) {
        old_ptr[i * 32] = 0;
    }
    exfat_write_cluster(exfat, old_priv->first_cluster, old_buf);
    
    // Ищем место в новой директории
    uint8_t new_buf[exfat->bytes_per_cluster];
    exfat_read_cluster(exfat, ((exfat_inode_private_t*)new_dir->i_private)->first_cluster, new_buf);
    
    // Ищем свободную запись
    int free_entry = -1;
    uint8_t *new_ptr = new_buf;
    for (int i = 0; i < exfat->bytes_per_cluster / 32; i++) {
        if (new_ptr[0] == 0) {
            free_entry = i;
            break;
        }
        new_ptr += 32;
    }
    
    if (free_entry == -1) {
        // Восстанавливаем старую запись
        memcpy(old_ptr, file_data, count * 32);
        exfat_write_cluster(exfat, old_priv->first_cluster, old_buf);
        vfs_free_inode(file);
        return -1;
    }
    
    // Копируем данные файла в новое место
    memcpy(new_buf + free_entry * 32, file_data, count * 32);
    
    // Обновляем имя в name entry
    uint8_t *name_ptr = new_buf + (free_entry + 2) * 32;  // после file и stream
    exfat_name_entry_t *name_entry = (exfat_name_entry_t*)name_ptr;
    memset(name_entry->name, 0, 30);
    for (int i = 0; i < strlen(new_name) && i < 15; i++) {
        name_entry->name[i] = new_name[i];
    }
    
    exfat_write_cluster(exfat, ((exfat_inode_private_t*)new_dir->i_private)->first_cluster, new_buf);
    
    // Обновляем dir_cluster и dir_entry в приватных данных файла
    file_priv->dir_cluster = ((exfat_inode_private_t*)new_dir->i_private)->first_cluster;
    file_priv->dir_entry = free_entry;
    
    vfs_free_inode(file);
    return 0;
}

// Чтение файла
static int exfat_read(vfs_inode_t *inode, uint64_t offset, void *buf, 
                      uint32_t size, uint32_t *read) {
    exfat_inode_private_t *priv = (exfat_inode_private_t*)inode->i_private;
    exfat_t *exfat = priv->exfat;
    
    if (offset >= inode->i_size) {
        *read = 0;
        return 0;
    }
    
    uint32_t to_read = size;
    if (offset + to_read > inode->i_size) {
        to_read = inode->i_size - offset;
    }
    
    uint8_t *buffer = (uint8_t*)buf;
    uint32_t done = 0;
    uint32_t cluster = priv->first_cluster;
    uint32_t cluster_size = exfat->bytes_per_cluster;
    
    // Пропускаем кластеры до смещения
    uint32_t skip_clusters = offset / cluster_size;
    for (uint32_t i = 0; i < skip_clusters && cluster >= 2; i++) {
        cluster = exfat_next_cluster(exfat, cluster);
    }
    
    if (cluster < 2) {
        *read = 0;
        return -1;
    }
    
    // Читаем
    uint32_t cluster_offset = offset % cluster_size;
    uint8_t temp_buf[cluster_size];
    
    while (done < to_read && cluster >= 2 && cluster != EXFAT_FAT_END) {
        exfat_read_cluster(exfat, cluster, temp_buf);
        
        uint32_t copy_start = cluster_offset;
        uint32_t copy_size = cluster_size - cluster_offset;
        if (copy_size > to_read - done) copy_size = to_read - done;
        
        memcpy(buffer + done, temp_buf + copy_start, copy_size);
        
        done += copy_size;
        cluster_offset = 0;
        cluster = exfat_next_cluster(exfat, cluster);
    }
    
    *read = done;
    return 0;
}

// Запись файла
static int exfat_write(vfs_inode_t *inode, uint64_t offset, const void *buf,
                       uint32_t size, uint32_t *written) {
    exfat_inode_private_t *priv = (exfat_inode_private_t*)inode->i_private;
    exfat_t *exfat = priv->exfat;
    
    *written = 0;
    
    if (size == 0) return 0;
    
    uint32_t cluster_size = exfat->bytes_per_cluster;
    uint8_t *buffer = (uint8_t*)buf;
    uint32_t to_write = size;
    uint32_t done = 0;
    
    // Определяем, сколько кластеров нужно
    uint32_t first_cluster = offset / cluster_size;
    uint32_t last_cluster = (offset + size - 1) / cluster_size;
    uint32_t clusters_needed = last_cluster - first_cluster + 1;
    
    // Получаем список кластеров
    uint32_t *clusters = (uint32_t*)malloc(clusters_needed * sizeof(uint32_t));
    if (!clusters) return -1;
    
    // Проходим по существующим кластерам
    uint32_t cluster = priv->first_cluster;
    uint32_t cluster_idx = 0;
    
    // Пропускаем до нужного кластера
    for (uint32_t i = 0; i < first_cluster && cluster >= 2; i++) {
        cluster = exfat_next_cluster(exfat, cluster);
    }
    
    // Собираем кластеры
    uint32_t c = 0;
    while (c < clusters_needed) {
        if (cluster < 2 || cluster == EXFAT_FAT_END) {
            // Нужен новый кластер
            uint32_t new_cluster = 0;
            for (uint32_t i = 2; i < exfat->fat_entries; i++) {
                if (exfat->fat_cache[i] == EXFAT_FAT_FREE) {
                    new_cluster = i;
                    break;
                }
            }
            if (new_cluster == 0) {
                free(clusters);
                return -1;  // нет места
            }
            
            // Связываем
            if (c == 0 && cluster_idx == 0) {
                // Это первый кластер файла
                priv->first_cluster = new_cluster;
            } else {
                // Связываем с предыдущим
                exfat->fat_cache[clusters[c-1]] = new_cluster;
            }
            exfat->fat_cache[new_cluster] = EXFAT_FAT_END;
            clusters[c] = new_cluster;
        } else {
            clusters[c] = cluster;
            cluster = exfat_next_cluster(exfat, cluster);
        }
        c++;
    }
    
    // Записываем данные
    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint32_t current_cluster = clusters[i];
        uint32_t cluster_offset = (i == 0) ? (offset % cluster_size) : 0;
        uint32_t write_size = cluster_size - cluster_offset;
        if (write_size > to_write - done) write_size = to_write - done;
        
        if (write_size == cluster_size) {
            // Пишем целый кластер
            exfat_write_cluster(exfat, current_cluster, buffer + done);
        } else {
            // Читаем, модифицируем, пишем
            uint8_t temp_buf[cluster_size];
            exfat_read_cluster(exfat, current_cluster, temp_buf);
            memcpy(temp_buf + cluster_offset, buffer + done, write_size);
            exfat_write_cluster(exfat, current_cluster, temp_buf);
        }
        
        done += write_size;
    }
    
    // Обновляем размер файла
    if (offset + done > inode->i_size) {
        inode->i_size = offset + done;
        
        // Обновляем stream entry в директории
        uint8_t dir_buf[cluster_size];
        exfat_read_cluster(exfat, priv->dir_cluster, dir_buf);
        
        uint8_t *ptr = dir_buf + priv->dir_entry * 32;
        exfat_stream_entry_t *stream = (exfat_stream_entry_t*)(ptr + 32);
        stream->data_length = inode->i_size;
        stream->valid_data_length = inode->i_size;
        
        exfat_write_cluster(exfat, priv->dir_cluster, dir_buf);
    }
    
    free(clusters);
    *written = done;
    exfat->fat_cache[0] |= 1;  // dirty
    
    return 0;
}

// Изменение размера
static int exfat_truncate(vfs_inode_t *inode, uint64_t new_size) {
    // TODO: реализовать truncate
    return -1;
}

// Синхронизация
static int exfat_sync(vfs_inode_t *inode) {
    exfat_inode_private_t *priv = (exfat_inode_private_t*)inode->i_private;
    exfat_t *exfat = priv->exfat;
    
    if (exfat->fat_cache[0] & 1) {  // dirty
        // Записываем FAT обратно на диск
        for (uint32_t i = 0; i < exfat->vbr.fat_length; i++) {
            uint64_t sector = exfat->vbr.fat_offset + i;
            blockdev_write(exfat->dev, sector, exfat->bytes_per_sector / 512,
                          (uint8_t*)exfat->fat_cache + i * exfat->bytes_per_sector);
        }
        exfat->fat_cache[0] &= ~1;  // clear dirty
    }
    
    return 0;
}

static int exfat_chmod(vfs_inode_t *inode, uint32_t mode) {
    if (!inode) return -1;
    
    exfat_inode_private_t *priv = (exfat_inode_private_t*)inode->i_private;
    exfat_t *exfat = priv->exfat;
    
    // Конвертируем UNIX-подобные права в атрибуты exFAT
    uint16_t attributes = 0;
    
    // Тип файла
    if (inode->i_mode == FT_DIR) {
        attributes |= EXFAT_ATTR_DIRECTORY;
    }
    
    // Права на запись (для владельца)
    if (!(mode & 0200)) {  // Нет права на запись для владельца
        attributes |= EXFAT_ATTR_READ_ONLY;
    }
    
    // Читаем кластер директории, где находится запись о файле
    uint8_t buf[exfat->bytes_per_cluster];
    exfat_read_cluster(exfat, priv->dir_cluster, buf);
    
    // Находим запись файла
    uint8_t *ptr = buf + priv->dir_entry * 32;
    exfat_file_entry_t *file_entry = (exfat_file_entry_t*)ptr;
    
    // Обновляем атрибуты
    file_entry->file_attributes = attributes;
    
    // Записываем обратно
    exfat_write_cluster(exfat, priv->dir_cluster, buf);
    
    // Обновляем режим в иноде
    inode->i_mode = mode;
    
    return 0;
}

// Статистика
static int exfat_stat(vfs_inode_t *inode, void *stat_buf) {
    vfs_stat_t *stat = (vfs_stat_t*)stat_buf;
    return vfs_stat(inode, stat);
}

static int exfat_readdir(vfs_inode_t *dir, uint64_t *pos, char *name, 
                         uint32_t *name_len, uint32_t *type) {
    // Проверка аргументов
    if (!dir || !pos || !name || !name_len || !type) return -1;
    if (dir->i_mode != FT_DIR) return -1;
    
    exfat_inode_private_t *priv = (exfat_inode_private_t*)dir->i_private;
    exfat_t *exfat = priv->exfat;
    
    // Текущая позиция = номер записи
    uint32_t entry_index = *pos;
    uint32_t entries_per_cluster = exfat->bytes_per_cluster / 32;
    
    // Какой кластер и смещение в нем
    uint32_t cluster_index = entry_index / entries_per_cluster;
    uint32_t cluster_offset = entry_index % entries_per_cluster;
    
    // Находим нужный кластер
    uint32_t cluster = priv->first_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        if (cluster < 2 || cluster == EXFAT_FAT_END) {
            *pos = 0;
            return -1;
        }
        cluster = exfat_next_cluster(exfat, cluster);
    }
    
    if (cluster < 2 || cluster == EXFAT_FAT_END) {
        *pos = 0;
        return -1;
    }
    
    // Читаем кластер
    uint8_t buf[exfat->bytes_per_cluster];
    if (exfat_read_cluster(exfat, cluster, buf) != 0) {
        return -1;
    }
    
    // Ищем запись, начиная с cluster_offset
    for (uint32_t i = cluster_offset; i < entries_per_cluster; i++) {
        uint8_t *entry_ptr = buf + i * 32;
        uint8_t entry_type = entry_ptr[0];
        
        // Конец директории
        if (entry_type == 0) {
            *pos = 0;
            return -1;
        }
        
        // Это файл или директория?
        if (entry_type == 0x85) {  // EXFAT_ENTRY_FILE
            exfat_file_entry_t *file_entry = (exfat_file_entry_t*)entry_ptr;
            
            // Получаем количество дополнительных записей
            uint8_t secondary_count = file_entry->secondary_count;
            
            // Ищем stream entry (идет сразу после file entry)
            uint8_t *stream_ptr = entry_ptr + 32;
            exfat_stream_entry_t *stream = NULL;
            
            if (stream_ptr[0] == 0xC0) {  // EXFAT_ENTRY_STREAM
                stream = (exfat_stream_entry_t*)stream_ptr;
            }
            
            if (!stream) {
                // Пропускаем все записи этой группы
                i += secondary_count;
                continue;
            }
            
            // Ищем name entry (идет после stream)
            uint8_t *name_ptr = stream_ptr + 32;
            int name_pos = 0;
            
            for (int j = 0; j < secondary_count - 1; j++) {
                if (name_ptr[0] == 0xC1) {  // EXFAT_ENTRY_NAME
                    exfat_name_entry_t *name_entry = (exfat_name_entry_t*)name_ptr;
                    
                    // Копируем имя (только ASCII символы)
                    for (int k = 0; k < 15; k++) {
                        uint16_t c = name_entry->name[k];
                        if (c == 0) break;
                        if (c < 128 && name_pos < 255) {
                            name[name_pos++] = (char)c;
                        }
                    }
                }
                name_ptr += 32;
            }
            
            name[name_pos] = '\0';
            *name_len = name_pos;
            
            // Определяем тип
            if (file_entry->file_attributes & 0x10) {  // EXFAT_ATTR_DIRECTORY
                *type = FT_DIR;
            } else {
                *type = FT_REG_FILE;
            }
            
            // Обновляем позицию на СЛЕДУЮЩУЮ запись после этой группы
            *pos = entry_index + 1 + secondary_count;
            
            return 0;
        }
        
        // Пропускаем вспомогательные записи (они будут обработаны вместе с основной)
        if (entry_type == 0xC0 || entry_type == 0xC1) {
            continue;
        }
        
        // Для неизвестных типов просто увеличиваем счетчик
        entry_index++;
    }
    
    // Если дошли сюда - записей больше нет
    *pos = 0;
    return -1;
}


static int exfat_parent(vfs_inode_t *inode, vfs_inode_t **parent) {
    if (!inode || !parent) return -1;
    
    exfat_inode_private_t *priv = (exfat_inode_private_t*)inode->i_private;
    exfat_t *exfat = priv->exfat;
    
    // Если это корень
    if (priv->first_cluster == exfat->root_cluster) {
        *parent = inode;  // Корень ссылается сам на себя
        return 0;
    }
    
    // Создаем инод для родителя
    vfs_inode_t *parent_inode = vfs_alloc_inode();
    exfat_inode_private_t *parent_priv = (exfat_inode_private_t*)malloc(sizeof(exfat_inode_private_t));
    
    parent_priv->exfat = exfat;
    parent_priv->first_cluster = priv->parent_cluster;
    parent_priv->data_length = 0;
    parent_priv->dir_cluster = 0;  // Для родителя не нужно
    parent_priv->dir_entry = 0;
    parent_priv->parent_cluster = exfat->root_cluster;  // Упрощенно
    
    parent_inode->i_mode = FT_DIR;
    parent_inode->i_size = 0;
    parent_inode->i_private = parent_priv;
    parent_inode->i_op = inode->i_op;
    parent_inode->i_fop = inode->i_fop;
    
    *parent = parent_inode;
    return 0;
}

// ==================== МОНТИРОВАНИЕ ====================

static vfs_operations_t exfat_i_op = {
    .lookup = exfat_lookup,
    .create = exfat_create,
    .unlink = exfat_unlink,
    .mkdir = exfat_mkdir,
    .rmdir = exfat_rmdir,
    .rename = exfat_rename,
    .chmod = exfat_chmod,
    .stat = exfat_stat,
    .readdir = exfat_readdir,
    .parent = exfat_parent,
};

static vfs_file_operations_t exfat_f_op = {
    .read = exfat_read,
    .write = exfat_write,
    .truncate = exfat_truncate,
    .sync = exfat_sync,
};

static int exfat_mount(blockdev_t *dev, vfs_inode_t **root) {
    tio_printf("[exFAT] Mounting...\n");
    
    exfat_t *exfat = (exfat_t*)malloc(sizeof(exfat_t));
    if (!exfat) return -1;
    
    memset(exfat, 0, sizeof(exfat_t));
    exfat->dev = dev;
    
    // Читаем VBR (сектор 0)
    uint8_t sector[512];
    if (exfat_read_sectors(exfat, 0, 1, sector) != 0) {
        free(exfat);
        return -1;
    }
    
    exfat_vbr_t *vbr = (exfat_vbr_t*)sector;
    
    // Проверяем сигнатуру
    if (memcmp(vbr->fs_name, "EXFAT   ", 8) != 0) {
        tio_printf("[exFAT] Not an exFAT volume (bad signature)\n");
        free(exfat);
        return -1;
    }
    
    exfat->vbr = *vbr;
    exfat->bytes_per_sector = 1 << vbr->bytes_per_sector_shift;
    exfat->sectors_per_cluster = 1 << vbr->sectors_per_cluster_shift;
    exfat->bytes_per_cluster = exfat->bytes_per_sector * exfat->sectors_per_cluster;
    
    exfat->fat_start = (uint64_t)vbr->fat_offset * exfat->bytes_per_sector;
    exfat->cluster_heap_start = (uint64_t)vbr->cluster_heap_offset * exfat->bytes_per_sector;
    exfat->root_cluster = vbr->root_dir_cluster;
    
    tio_printf("[exFAT] Sector size: %d, Cluster size: %d, Root cluster: %d\n",
               exfat->bytes_per_sector, exfat->bytes_per_cluster, exfat->root_cluster);
    
    // Выделяем память под FAT
    uint32_t fat_size_bytes = vbr->fat_length * exfat->bytes_per_sector;
    exfat->fat_entries = fat_size_bytes / 4;
    exfat->fat_cache = (uint32_t*)malloc(fat_size_bytes);
    if (!exfat->fat_cache) {
        free(exfat);
        return -1;
    }
    
    // Читаем FAT
    for (uint32_t i = 0; i < vbr->fat_length; i++) {
        uint64_t sector_num = vbr->fat_offset + i;
        exfat_read_sectors(exfat, sector_num, 1, 
                          (uint8_t*)exfat->fat_cache + i * exfat->bytes_per_sector);
    }
    
    // Создаем корневой инод
    vfs_inode_t *root_inode = vfs_alloc_inode();
    exfat_inode_private_t *priv = (exfat_inode_private_t*)malloc(sizeof(exfat_inode_private_t));
    
    priv->exfat = exfat;
    priv->first_cluster = exfat->root_cluster;
    priv->data_length = 0;
    priv->dir_cluster = exfat->root_cluster;
    priv->dir_entry = 0;
    priv->parent_cluster = priv->first_cluster;
    
    root_inode->i_mode = FT_DIR;
    root_inode->i_uid = 0;
    root_inode->i_gid = 0;
    root_inode->i_size = 0;
    root_inode->i_ino = 2;
    root_inode->i_private = priv;
    
    root_inode->i_op = &exfat_i_op;
    root_inode->i_fop = &exfat_f_op;
    
    *root = root_inode;
    
    tio_printf("[exFAT] Mounted successfully\n");
    return 0;
}

static int exfat_unmount(vfs_inode_t *root) {
    if (!root) return -1;
    
    exfat_inode_private_t *priv = (exfat_inode_private_t*)root->i_private;
    exfat_t *exfat = priv->exfat;
    
    if (exfat->fat_cache) {
        // TODO: записать dirty записи FAT обратно
        free(exfat->fat_cache);
    }
    
    free(priv);
    free(exfat);
    vfs_free_inode(root);
    
    return 0;
}

// Регистрация файловой системы
static file_system_t exfat_fs = {
    .name = "exfat",
    .mount = exfat_mount,
    .unmount = exfat_unmount,
    .next = NULL
};

void exfat_init(void) {
    vfs_register_fs(&exfat_fs);
    tio_printf("[exFAT] Driver initialized\n");
}

int exfat_format(blockdev_t *dev) {
    if (!dev || dev->status != BLOCKDEV_READY) {
        tio_printf("[exFAT] Device not ready\n");
        return -1;
    }
    
    tio_printf("[exFAT] Formatting %s...\n", dev->name);
    
    // Определяем параметры
    uint32_t sector_size = dev->sector_size;
    uint64_t total_sectors = dev->total_sectors;
    
    // Выбираем размер кластера (4КБ - 32КБ в зависимости от размера диска)
    uint8_t sectors_per_cluster_shift;
    if (total_sectors < 0x100000) { // Меньше 512МБ
        sectors_per_cluster_shift = 0; // 1 сектор = 512 байт
    } else if (total_sectors < 0x400000) { // Меньше 2ГБ
        sectors_per_cluster_shift = 1; // 2 сектора = 1КБ
    } else if (total_sectors < 0x1000000) { // Меньше 8ГБ
        sectors_per_cluster_shift = 3; // 8 секторов = 4КБ
    } else {
        sectors_per_cluster_shift = 6; // 64 сектора = 32КБ
    }
    
    uint32_t sectors_per_cluster = 1 << sectors_per_cluster_shift;
    uint32_t cluster_size = sector_size * sectors_per_cluster;
    
    // Рассчитываем размер FAT
    uint64_t total_clusters = total_sectors / sectors_per_cluster;
    uint32_t fat_entries = total_clusters + 2; // +2 для зарезервированных
    uint32_t fat_sectors = (fat_entries * 4 + sector_size - 1) / sector_size;
    
    // Смещения
    uint32_t fat_offset = 24; // Секторов после VBR
    uint32_t cluster_heap_offset = fat_offset + fat_sectors;
    uint32_t root_cluster = 2; // Первый кластер
    
    tio_printf("[exFAT] Total sectors: %lu, Cluster size: %u, FAT sectors: %u\n",
               total_sectors, cluster_size, fat_sectors);
    
    // 1. Создаем и записываем VBR
    exfat_vbr_t vbr;
    memset(&vbr, 0, sizeof(vbr));
    
    // Jump boot
    vbr.jump_boot[0] = 0xEB;
    vbr.jump_boot[1] = 0x76;
    vbr.jump_boot[2] = 0x90;
    
    // FS Name
    memcpy(vbr.fs_name, "EXFAT   ", 8);
    
    // Partition info
    vbr.partition_offset = 0;
    vbr.volume_length = total_sectors;
    vbr.fat_offset = fat_offset;
    vbr.fat_length = fat_sectors;
    vbr.cluster_heap_offset = cluster_heap_offset;
    vbr.cluster_count = total_clusters;
    vbr.root_dir_cluster = root_cluster;
    vbr.volume_serial = 0x12345678; // TODO: генерация случайного
    vbr.fs_revision = 0x0100; // Version 1.0
    vbr.volume_flags = 0;
    vbr.bytes_per_sector_shift = 9; // 512 bytes (2^9)
    vbr.sectors_per_cluster_shift = sectors_per_cluster_shift;
    vbr.number_of_fats = 1;
    vbr.drive_select = 0x80;
    vbr.percent_in_use = 0;
    
    // Boot code and signature
    vbr.boot_code[0] = 0xF4; // hlt
    vbr.signature = 0xAA55;
    
    // Записываем VBR в сектор 0
    if (blockdev_write(dev, 0, 1, &vbr) != 0) {
        tio_printf("[exFAT] Failed to write VBR\n");
        return -1;
    }
    
    // 2. Создаем и записываем FAT
    uint32_t *fat = (uint32_t*)malloc(fat_sectors * sector_size);
    if (!fat) return -1;
    
    memset(fat, 0, fat_sectors * sector_size);
    
    // Зарезервированные кластеры
    fat[0] = 0xFFFFFFF8; // Media descriptor
    fat[1] = 0xFFFFFFFF; // EOC marker
    
    // Помечаем кластер корневой директории
    fat[root_cluster] = 0xFFFFFFFF; // End of chain
    
    // Записываем FAT
    for (uint32_t i = 0; i < fat_sectors; i++) {
        if (blockdev_write(dev, fat_offset + i, 1, 
                          (uint8_t*)fat + i * sector_size) != 0) {
            free(fat);
            tio_printf("[exFAT] Failed to write FAT\n");
            return -1;
        }
    }
    free(fat);
    
    // 3. Инициализируем корневую директорию
    uint8_t *cluster_buf = (uint8_t*)malloc(cluster_size);
    if (!cluster_buf) return -1;
    
    memset(cluster_buf, 0, cluster_size);
    
    // Записываем корневую директорию
    uint64_t root_sector = cluster_heap_offset + (root_cluster - 2) * sectors_per_cluster;
    if (blockdev_write(dev, root_sector, sectors_per_cluster, cluster_buf) != 0) {
        free(cluster_buf);
        tio_printf("[exFAT] Failed to write root directory\n");
        return -1;
    }
    free(cluster_buf);
    
    tio_printf("[exFAT] Format complete\n");
    return 0;
}

int exfat_get_name(vfs_inode_t *inode, char *name, int max_len) {
    if (!inode || !name) return -1;
    
    exfat_inode_private_t *priv = (exfat_inode_private_t*)inode->i_private;
    exfat_t *exfat = priv->exfat;
    
    // Если это корень
    if (priv->first_cluster == exfat->root_cluster) {
        strcpy(name, "");
        return 0;
    }
    
    // Читаем директорию, где лежит запись об этом иноде
    uint8_t buf[exfat->bytes_per_cluster];
    if (exfat_read_cluster(exfat, priv->dir_cluster, buf) != 0) {
        return -1;
    }
    
    // Находим запись
    uint8_t *ptr = buf + priv->dir_entry * 32;
    
    // Проверяем, что это файловая запись
    if (ptr[0] != 0x85) return -1;
    
    exfat_file_entry_t *file = (exfat_file_entry_t*)ptr;
    uint8_t count = file->secondary_count;
    
    // Ищем name entry
    uint8_t *name_ptr = ptr + 32 + 32;  // Пропускаем file и stream
    int name_pos = 0;
    
    for (int i = 0; i < count - 1 && name_pos < max_len - 1; i++) {
        if (name_ptr[0] == 0xC1) {
            exfat_name_entry_t *name_entry = (exfat_name_entry_t*)name_ptr;
            for (int j = 0; j < 15; j++) {
                uint16_t c = name_entry->name[j];
                if (c == 0) break;
                if (c < 128) name[name_pos++] = (char)c;
            }
        }
        name_ptr += 32;
    }
    
    name[name_pos] = '\0';
    return 0;
}