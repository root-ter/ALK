// fs/elf/elfload.h
#ifndef ELFLOAD_H
#define ELFLOAD_H

#include <stdint.h>
#include "../../fs/vfs/vfs.h"

// Загрузить и запустить ELF программу
int elf_load_and_exec(vfs_file_t *file, const char *name, int argc, char **argv);

// Проверить, является ли файл ELF
int elf_check(vfs_file_t *file);

#endif