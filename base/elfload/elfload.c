#include "elf.h"
#include "elfload.h"
#include "../mem/mem.h"
#include "../mem/paging.h"
#include "../sched/sched.h"
#include "../term/tio.h"
#include "../../libc/string.h"

// Проверка ELF магии
static int elf_check_magic(elf64_hdr_t *hdr) {
    return (hdr->e_ident[0] == 0x7F &&
            hdr->e_ident[1] == 'E' &&
            hdr->e_ident[2] == 'L' &&
            hdr->e_ident[3] == 'F');
}

// Проверить, является ли файл ELF
int elf_check(vfs_file_t *file) {
    if (!file) return -1;
    
    elf64_hdr_t hdr;
    uint32_t read;
    
    vfs_seek(file, 0, 0);
    if (vfs_read(file, &hdr, sizeof(hdr), &read) != 0 || read != sizeof(hdr)) {
        return -1;
    }
    
    return elf_check_magic(&hdr) ? 0 : -1;
}

// Загрузить и запустить ELF программу
int elf_load_and_exec(vfs_file_t *file, const char *name, int argc, char **argv) {
    if (!file || !name) return -1;
    
    elf64_hdr_t hdr;
    uint32_t read;
    
    // Читаем заголовок
    vfs_seek(file, 0, 0);
    if (vfs_read(file, &hdr, sizeof(hdr), &read) != 0 || read != sizeof(hdr)) {
        tio_printf("[ELF] Failed to read header\n");
        return -1;
    }
    
    if (!elf_check_magic(&hdr)) {
        tio_printf("[ELF] Invalid magic\n");
        return -1;
    }
    
    if (hdr.e_machine != EM_X86_64) {
        tio_printf("[ELF] Unsupported machine: %d\n", hdr.e_machine);
        return -1;
    }
    
    tio_printf("[ELF] Loading program '%s': entry=0x%lx, phnum=%d\n", 
               name, hdr.e_entry, hdr.e_phnum);
    
    // Вычисляем общий размер и базовый адрес
    uint64_t min_vaddr = 0xFFFFFFFFFFFFFFFF;
    uint64_t max_vaddr = 0;
    
    for (int i = 0; i < hdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        vfs_seek(file, hdr.e_phoff + i * hdr.e_phentsize, 0);
        if (vfs_read(file, &phdr, sizeof(phdr), &read) != 0 || read != sizeof(phdr)) {
            tio_printf("[ELF] Failed to read program header %d\n", i);
            return -1;
        }
        
        if (phdr.p_type == PT_LOAD) {
            if (phdr.p_vaddr < min_vaddr) min_vaddr = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > max_vaddr) 
                max_vaddr = phdr.p_vaddr + phdr.p_memsz;
        }
    }
    
    // Выравниваем по страницам
    min_vaddr &= ~0xFFF;
    max_vaddr = (max_vaddr + 0xFFF) & ~0xFFF;
    
    uint64_t total_size = max_vaddr - min_vaddr;
    
    tio_printf("[ELF] Program size: 0x%lx bytes (0x%lx pages)\n", 
               total_size, total_size / 4096);
    
    // Выделяем память для программы
    void *user_mem = malloc(total_size);
    if (!user_mem) {
        tio_printf("[ELF] Failed to allocate memory\n");
        return -1;
    }
    
    memset(user_mem, 0, total_size);
    
    // Загружаем сегменты
    for (int i = 0; i < hdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        vfs_seek(file, hdr.e_phoff + i * hdr.e_phentsize, 0);
        vfs_read(file, &phdr, sizeof(phdr), &read);
        
        if (phdr.p_type == PT_LOAD) {
            uint64_t offset_in_mem = phdr.p_vaddr - min_vaddr;
            uint8_t *dest = (uint8_t*)user_mem + offset_in_mem;
            
            tio_printf("[ELF] Loading segment: vaddr=0x%lx, offset=0x%lx, filesz=0x%lx, memsz=0x%lx\n",
                       phdr.p_vaddr, offset_in_mem, phdr.p_filesz, phdr.p_memsz);
            
            // Читаем данные из файла
            if (phdr.p_filesz > 0) {
                vfs_seek(file, phdr.p_offset, 0);
                vfs_read(file, dest, phdr.p_filesz, &read);
            }
            
            // .bss уже обнулен malloc'ом
        }
    }
    
    vfs_close(file);
    
    // Создаем пользовательскую задачу
    uint64_t pid = utask_create(
        (void (*)(void))hdr.e_entry,  // entry point
        8192,                          // stack size
        user_mem,                      // user memory
        total_size,                    // user memory size
        argc,                          // argc
        (uintptr_t)argv,                // argv pointer
        name                            // task name
    );
    
    if (pid == 0) {
        tio_printf("[ELF] Failed to create task\n");
        free(user_mem);
        return -1;
    }
    
    tio_printf("[ELF] Program '%s' started as PID %ld, entry at 0x%lx\n", 
               name, pid, hdr.e_entry);
    
    return pid;
}