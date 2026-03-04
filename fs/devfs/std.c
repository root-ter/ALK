#include "devfs.h"
#include "../../base/term/tio.h"
#include "../../drv/kbd/kbd.h"
#include "../../libc/string.h"

// Обработчик для stdout
static int stdout_write(const void *buf, size_t count, size_t *written) {
    const char *str = (const char*)buf;
    for (size_t i = 0; i < count; i++) {
        tio_putc(str[i]);
    }
    *written = count;
    return 0;
}

// Обработчик для stdin
static int stdin_read(void *buf, size_t count, size_t *read) {
    char *str = (char*)buf;
    size_t i = 0;
    
    while (i < count) {
        char c = kbd_getchar();
        if (c != -1) {
            str[i++] = c;
            if (c == '\n') break;
        } else {
            for (volatile int j = 0; j < 1000; j++);
        }
    }
    
    *read = i;
    return 0;
}

// Обработчик для stderr
static int stderr_write(const void *buf, size_t count, size_t *written) {
    const char *str = (const char*)buf;
    
    // Временный буфер для копирования (т.к. у нас нет гарантии, что строка нуль-терминирована)
    char temp[1024];
    size_t copy_len = count;
    if (copy_len > sizeof(temp) - 1) 
        copy_len = sizeof(temp) - 1;
    
    memcpy(temp, str, copy_len);
    temp[copy_len] = '\0';
    
    // Выводим через tio_printerr (красным)
    tio_printerr("%s", temp);
    
    *written = count;
    return 0;
}

// Драйверы
static device_driver_t stdin_driver = {
    .name = "stdin",
    .read = stdin_read,
    .write = NULL,
    .ioctl = NULL
};

static device_driver_t stdout_driver = {
    .name = "stdout",
    .read = NULL,
    .write = stdout_write,
    .ioctl = NULL
};

static device_driver_t stderr_driver = {
    .name = "stderr",
    .read = NULL,
    .write = stderr_write,
    .ioctl = NULL
};

// Инициализация std устройств в указанной директории
void devfs_init_std(vfs_inode_t *dir) {
    devfs_register_driver(&stdin_driver);
    devfs_register_driver(&stdout_driver);
    devfs_register_driver(&stderr_driver);
    
    devfs_mknod_in(dir, "stdin", FT_CHRDEV, &stdin_driver, NULL);
    devfs_mknod_in(dir, "stdout", FT_CHRDEV, &stdout_driver, NULL);
    devfs_mknod_in(dir, "stderr", FT_CHRDEV, &stderr_driver, NULL);
}