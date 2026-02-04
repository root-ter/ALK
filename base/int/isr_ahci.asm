[BITS 64]

global isr_ahci
extern ahci_irq_handler

isr_ahci:
    ; сохраняем регистры
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; вызываем C-обработчик
    call ahci_irq_handler

    ; восстанавливаем регистры
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    iretq

section .note.GNU-stack