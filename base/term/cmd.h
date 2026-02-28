#ifndef TERM_CMDS_H
#define TERM_CMDS_H

#include <stdint.h>
#include "../../fs/vfs/vfs.h"

// Выполнить команду
int term_execute_command(char* cmdline);

// Инициализация командной системы
// void term_commands_init(void); // Ничего не делает

// Обработка ввода команд (для использования в обработчике клавиатуры)
void term_handle_command_input(char input_char);

// Для VFS
char* build_path_recursive(vfs_inode_t *inode, char *buffer, int depth);

#endif