[BITS 32]

section .text
    align 8

; --------------------
; Multiboot2 header
; --------------------
align 8
mb2_header_start:
    dd 0xE85250D6
    dd 0x00000000
    dd mb2_header_end - mb2_header_start
    dd -(0xE85250D6 + 0x00000000 + (mb2_header_end - mb2_header_start))

align 8
info_request_tag:
    dw 1                         ; Type: Information request
    dw 0                         ; Flags
    dd info_request_tag_end - info_request_tag  ; Size
    dd 6                         ; Memory map tag type (MUST BE 6!)
info_request_tag_end:

; Framebuffer tag
align 8
fb_tag_start:
    dw 5
    dw 0
    dw fb_tag_end - fb_tag_start
    dd 0
    dd 0
    dd 32
fb_tag_end:

; End tag
align 8
end_tag:
    dw 0
    dw 0
    dd 8

mb2_header_end:

global start
global gdt
global tss_buffer
global stack64_top
extern kmain
extern _stack_end

start:
    cli
    
    ; Загружаем GDT
    lgdt [gdt_desc]
    
    ; Включаем PAE
    mov eax, cr4
    bts eax, 5
    mov cr4, eax
    
    ; Устанавливаем CR3 (адрес PML4)
    mov eax, pml4_table
    mov cr3, eax
    
    ; Включаем Long Mode через MSR
    mov ecx, 0xC0000080
    rdmsr
    bts eax, 8
    wrmsr
    
    ; Включаем пейджинг
    mov eax, cr0
    bts eax, 31
    mov cr0, eax
    
    ; Переход в long mode
    jmp 0x08:long_mode_entry

[BITS 64]
long_mode_entry:
    ; Сегментные регистры
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    
    ; Стек
    lea rsp, [rel _stack_end]
    and rsp, -16
    
    ; Вызов ядра (rbx = multiboot info)
    mov rdi, rbx
    call kmain

.hang64:
    hlt
    jmp .hang64

; -----------------------------------------------------------------------
; GDT
; -----------------------------------------------------------------------
align 8
gdt:
    dq 0x0000000000000000     ; Null
    dq 0x00AF9A000000FFFF     ; 0x08: Kernel Code
    dq 0x00AF92000000FFFF     ; 0x10: Kernel Data
    dq 0x00AFFA000000FFFF     ; 0x18: User Code
    dq 0x00AFF2000000FFFF     ; 0x20: User Data
    dq 0x0000000000000000     ; 0x28: TSS lower
    dq 0x0000000000000000     ; 0x30: TSS upper
gdt_end:

gdt_desc:
    dw gdt_end - gdt - 1
    dq gdt

; -----------------------------------------------------------------------
; Page Tables - УПРОЩЕННАЯ ВЕРСИЯ (работающая)
; -----------------------------------------------------------------------
section .data
align 4096

global pml4_table

pml4_table:
    dq pdp_low + 0x007     ; 0-512GB (пользователь)
    times 254 dq 0         ; Неиспользуемые записи
    dq pdp_high + 0x007    ; 0xFFFFFF8000000000-... (ядро)
    dq pml4_table + 0x007  ; Recursive mapping (последняя запись)

; PDPT для низкой памяти (пользователь)
align 4096
pdp_low:
    dq pd_table0 + 0x007   ; 0-1GB
    dq pd_table1 + 0x007   ; 1-2GB
    dq pd_table2 + 0x007   ; 2-3GB
    dq pd_table3 + 0x007   ; 3-4GB
    times 508 dq 0         ; Остальное не мапим пока

; PDPT для высокой памяти (ядро)
align 4096
pdp_high:
    times 510 dq 0         ; Заполняем позже
    dq pd_kernel0 + 0x007  ; -512GB до -511GB (ядро)
    dq pd_kernel1 + 0x007  ; -511GB до -510GB (ядро)

; PD таблицы для ядра (2MB страницы)
align 4096
pd_kernel0:
%assign i 0
%rep 512
    dq (i * 0x200000) + 0x083  ; Identity map первые 1GB
%assign i i+1
%endrep

pd_kernel1:
%assign i 512
%rep 512
    dq (i * 0x200000) + 0x083  ; Следующие 1GB
%assign i i+1
%endrep

; PD tables (2MB страницы) для 0-4GB
; Просто identity mapping, NX=0 для всех (можно исполнять)
align 4096
pd_table0:  ; 0-1GB
%assign i 0
%rep 512
    dq (i * 0x200000) + 0x083  ; P=1, RW=1, PS=1 (2MB), NX=0
%assign i i+1
%endrep

align 4096
pd_table1:  ; 1-2GB
%assign i 512
%rep 512
    dq (i * 0x200000) + 0x083
%assign i i+1
%endrep

align 4096
pd_table2:  ; 2-3GB
%assign i 1024
%rep 512
    dq (i * 0x200000) + 0x083
%assign i i+1
%endrep

align 4096
pd_table3:  ; 3-4GB
%assign i 1536
%rep 512
    dq (i * 0x200000) + 0x083
%assign i i+1
%endrep

section .note.GNU-stack
