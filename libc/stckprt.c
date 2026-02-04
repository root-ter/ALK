#include <stdint.h>
#include "../base/rsod/rsod.h"

/* Глобальный guard, который GCC читает */
uintptr_t __stack_chk_guard = 0xBAAAD00Du;

/* Простой вывод + останов ядра */
static void __attribute__((noreturn)) kstack_panic(void)
{
    /* Глушим всё */
    rsod("STACK_CRITICAL_ERROR", "STACK");
    __builtin_unreachable();
}

/* Вызывается GCC при несоответствии канареек */
void __attribute__((noreturn)) __stack_chk_fail(void)
{
    kstack_panic();
}

/* Локальная версия, на i386/ELF часто зовётся именно так */
void __attribute__((noreturn)) __stack_chk_fail_local(void)
{
    kstack_panic();
}
