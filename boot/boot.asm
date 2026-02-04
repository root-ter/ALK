[BITS 32]

section .text
    align 8

    ; --------------------
    ; Multiboot2 header (только один!)
    ; --------------------
    align 8
mb2_header_start:
    dd 0xE85250D6              ; magic (multiboot2)
    dd 0x00000000              ; architecture (0 = i386)
    dd mb2_header_end - mb2_header_start ; header length
    dd -(0xE85250D6 + 0x00000000 + (mb2_header_end - mb2_header_start)) ; checksum

    ; --- Framebuffer tag (должен идти ПЕРЕД Memory Map!) ---
    align 8
fb_tag_start:
    dw 5                       ; tag type = 5 (framebuffer)
    dw 0                       ; flags
    dw fb_tag_end - fb_tag_start ; size
    dd 0                       ; width (0 = preferred)
    dd 0                       ; height (0 = preferred)
    dd 32                      ; bpp
fb_tag_end:

    ; --- End tag ---
    align 8
end_tag:
    dw 0                       ; type = 0 (end)
    dw 0                       ; flags
    dd 8                       ; size = 8

mb2_header_end:

global start
global gdt
global tss_buffer
global stack64_top
extern kmain

start:
    cli
    lgdt [gdt_desc]
    
    ; Enable PAE
    mov eax, cr4
    bts eax, 5
    mov cr4, eax
    
    ; Set CR3
    mov eax, pml4_table
    mov cr3, eax
    
    ; Enable LME
    mov ecx, 0xC0000080
    rdmsr
    bts eax, 8
    wrmsr
    
    ; Enable paging
    mov eax, cr0
    bts eax, 31
    mov cr0, eax
    
    ; Jump to 64-bit
    jmp 0x08:long_mode_entry

[BITS 64]
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    
    ; Stack
    lea rsp, [rel stack64_top]
    and rsp, -16
    
    ; Call kernel
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
; Stack
; -----------------------------------------------------------------------
section .bss
align 16
stack64_bottom:
    resb 65536
stack64_top:

; TSS
align 16
tss_buffer:
    resb 104

; -----------------------------------------------------------------------
; Page Tables with HIGH MEMORY support (>4GB) and NX bit
; -----------------------------------------------------------------------
section .data
align 4096

; PML4 Table with recursive mapping
pml4_table:
    ; PML4[0] -> PDPT for first 512GB
    dq pdpt_low_mem + 0x007
    
    ; PML4[1] -> PDPT for high memory (4GB-256GB)
    dq pdpt_high_mem + 0x007
    
    ; PML4[510] -> Recursive mapping
    times 508 dq 0
    dq pml4_table + 0x007
    
    dq 0

; PDPT for LOW memory (0-4GB)
align 4096
pdpt_low_mem:
    ; 4 entries = 4GB
    dq pd_table0 + 0x007      ; 0-1GB
    dq pd_table1 + 0x007      ; 1-2GB
    dq pd_table2 + 0x007      ; 2-3GB
    dq pd_table3 + 0x007      ; 3-4GB
    times 508 dq 0

; PDPT for HIGH memory (4GB-68GB) - 64 entries = 64GB
align 4096
pdpt_high_mem:
%assign i 0
%rep 64  ; 64GB of high memory
    dq pd_tables_high + (i * 4096) + 0x007
%assign i i+1
%endrep
times (512 - 64) * 8 db 0

; PD tables for LOW memory (2MB pages)
; ВАЖНО: Оставляем исполняемыми ВСЕ страницы низкой памяти
; так как код ядра может быть разбросан по разным адресам
align 4096
pd_table0:  ; 0x00000000 .. 0x3FFFFFFF
%assign i 0
%rep 512
    ; Бит 63 (NX) = 0 для ВСЕХ страниц в low memory
    ; Это безопасно, так как user mode не может писать в kernel space
    dq (i * 0x200000) + 0x087  ; NX=0 для всех
%assign i i+1
%endrep

align 4096
pd_table1:  ; 0x40000000 .. 0x7FFFFFFF
%assign i 512
%rep 512
    ; Здесь тоже оставляем NX=0 на всякий случай
    ; Можно добавить NX позже через mmap
    dq (i * 0x200000) + 0x087  ; NX=0
%assign i i+1
%endrep

align 4096
pd_table2:  ; 0x80000000 .. 0xBFFFFFFF
%assign i 1024
%rep 512
    dq (i * 0x200000) + 0x087  ; NX=0
%assign i i+1
%endrep

align 4096
pd_table3:  ; 0xC0000000 .. 0xFFFFFFFF
%assign i 1536
%rep 512
    dq (i * 0x200000) + 0x087  ; NX=0
%assign i i+1
%endrep

; PD tables for HIGH memory (4GB+)
; Здесь уже можно безопасно включать NX, так как high memory
; обычно используется только для данных (DMA, большие буферы)
align 4096
pd_tables_high:
%assign pd_index 0
%rep 64  ; 64 PD tables = 64GB
pd_table_high_%[pd_index]:
    %assign entry 0
    %rep 512  ; 512 entries per PD = 1GB
        ; Address: 4GB + (pd_index * 1GB) + (entry * 2MB)
        ; High memory: все страницы неисполняемые (NX=1)
        dq (0x100000000 + (pd_index * 0x40000000) + (entry * 0x200000)) + 0x087 + (1 << 63)
    %assign entry entry+1
    %endrep
%assign pd_index pd_index+1
%endrep

section .note.GNU-stack