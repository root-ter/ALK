[BITS 64]

global page_fault_simple
extern term_printf
extern term

section .text

page_fault_simple:
    cli
    push rax
    
    ; Получаем адрес ошибки
    mov rax, cr2
    
    ; Выводим сообщение
    push rdi
    push rsi
    
    mov rdi, [rel term_ptr]
    mov rsi, str_fault
    call term_printf
    
    mov rdi, [rel term_ptr]
    mov rsi, rax        ; fault address
    mov rdx, [rsp + 24] ; error code (уже в стеке от CPU)
    call term_printf
    
    pop rsi
    pop rdi
    pop rax
    
    ; Просто убиваем процесс если fault в user mode
    test qword [rsp + 8], 0x04  ; Проверяем бит USER в error code
    jz .kernel_fault
    
    ; User fault - возвращаемся в планировщик с кодом ошибки
    add rsp, 8  ; пропускаем saved rax
    add rsp, 16 ; пропускаем error code и int number
    mov rax, 0xDEAD  ; Специальный код для планировщика
    iretq
    
.kernel_fault:
    ; Kernel fault - зависаем
    hlt
    jmp .kernel_fault

section .data
term_ptr: dq 0
str_fault: db "[FAULT] Addr: 0x%llx, Code: %d", 10, 0

section .note.GNU-stack