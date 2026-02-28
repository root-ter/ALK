#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// Типы ELF файлов
#define ET_NONE     0
#define ET_REL      1
#define ET_EXEC     2
#define ET_DYN      3
#define ET_CORE     4

// Машины
#define EM_NONE     0
#define EM_386      3
#define EM_X86_64   62

// Типы секций
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_NOTE        7
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHT_SHLIB       10
#define SHT_DYNSYM      11

// Флаги секций
#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

// Типы программных сегментов
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7

// Флаги сегментов
#define PF_X            0x1
#define PF_W            0x2
#define PF_R            0x4

// ELF заголовок (64-bit)
typedef struct {
    unsigned char e_ident[16];     // Магические байты
    uint16_t      e_type;           // Тип файла
    uint16_t      e_machine;        // Архитектура
    uint32_t      e_version;        // Версия
    uint64_t      e_entry;          // Точка входа
    uint64_t      e_phoff;          // Смещение program header
    uint64_t      e_shoff;          // Смещение section header
    uint32_t      e_flags;          // Флаги
    uint16_t      e_ehsize;         // Размер ELF заголовка
    uint16_t      e_phentsize;      // Размер program header entry
    uint16_t      e_phnum;          // Количество program header entries
    uint16_t      e_shentsize;      // Размер section header entry
    uint16_t      e_shnum;          // Количество section header entries
    uint16_t      e_shstrndx;       // Индекс секции с именами
} __attribute__((packed)) elf64_hdr_t;

// Program header (64-bit)
typedef struct {
    uint32_t p_type;                 // Тип сегмента
    uint32_t p_flags;                // Флаги
    uint64_t p_offset;               // Смещение в файле
    uint64_t p_vaddr;                // Виртуальный адрес
    uint64_t p_paddr;                // Физический адрес (обычно не используется)
    uint64_t p_filesz;               // Размер в файле
    uint64_t p_memsz;                // Размер в памяти
    uint64_t p_align;                // Выравнивание
} __attribute__((packed)) elf64_phdr_t;

// Section header (64-bit)
typedef struct {
    uint32_t sh_name;                 // Имя (индекс в strtab)
    uint32_t sh_type;                 // Тип секции
    uint64_t sh_flags;                // Флаги
    uint64_t sh_addr;                 // Адрес в памяти
    uint64_t sh_offset;               // Смещение в файле
    uint64_t sh_size;                 // Размер
    uint32_t sh_link;                 // Связанная секция
    uint32_t sh_info;                 // Доп. информация
    uint64_t sh_addralign;            // Выравнивание
    uint64_t sh_entsize;              // Размер элемента (для таблиц)
} __attribute__((packed)) elf64_shdr_t;

#endif