#include "../term/term.h"
#include "../../drv/fb/fb.h"
#include "rsod.h"
#include "../../libc/string.h"
#include "../sched/sched.h"
#include "../../drv/sound/pcs/pcs.h"
#include "../../drv/acpi/acpi.h"
#include "../time/timer.h"
#include "../../drv/kbd/kbd.h"
#include <stdint.h>
#include <stdbool.h>

extern term_t* term;
extern framebuffer_t fb;

int rsod_is_running = 0;

static void ulltohex(uint64_t value, char* buffer, int width) {
    const char hex_digits[] = "0123456789ABCDEF";
    int i;
    
    for (i = width - 1; i >= 0; i--) {
        buffer[i] = hex_digits[value & 0xF];
        value >>= 4;
    }
    buffer[width] = '\0';
}

static void utohex(uint32_t value, char* buffer, int width) {
    ulltohex((uint64_t)value, buffer, width);
}

static void ustohex(uint16_t value, char* buffer, int width) {
    ulltohex((uint64_t)value, buffer, width);
}

// Альтернативная функция для печати hex
static void format_hex(uint64_t value, char* out, int leading_zeros) {
    const char* digits = "0123456789ABCDEF";
    int i;
    
    for (i = leading_zeros - 1; i >= 0; i--) {
        out[i] = digits[value & 0xF];
        value >>= 4;
    }
    out[leading_zeros] = '\0';
}

static void get_registers(registers_state *regs)
{
    if (!regs)
        return;

    asm volatile(
        "movq %%rax, 0x00(%0)\n\t"
        "movq %%rbx, 0x08(%0)\n\t"
        "movq %%rcx, 0x10(%0)\n\t"
        "movq %%rdx, 0x18(%0)\n\t"
        "movq %%rsi, 0x20(%0)\n\t"
        "movq %%rdi, 0x28(%0)\n\t"
        "movq %%rbp, 0x30(%0)\n\t"
        "movq %%r8,  0x38(%0)\n\t"
        "movq %%r9,  0x40(%0)\n\t"
        "movq %%r10, 0x48(%0)\n\t"
        "movq %%r11, 0x50(%0)\n\t"
        "movq %%r12, 0x58(%0)\n\t"
        "movq %%r13, 0x60(%0)\n\t"
        "movq %%r14, 0x68(%0)\n\t"
        "movq %%r15, 0x70(%0)\n\t"
        "movq %%rsp, %%rax\n\t"
        "movq %%rax, 0x78(%0)\n\t"
        "leaq (%%rip), %%rax\n\t"
        "movq %%rax, 0x80(%0)\n\t"
        "pushfq\n\t"
        "popq %%rax\n\t"
        "movq %%rax, 0x88(%0)\n\t"
        "mov %%cs, %%ax\n\t"
        "movw %%ax, 0x90(%0)\n\t"
        "mov %%ds, %%ax\n\t"
        "movw %%ax, 0x92(%0)\n\t"
        "mov %%es, %%ax\n\t"
        "movw %%ax, 0x94(%0)\n\t"
        "mov %%fs, %%ax\n\t"
        "movw %%ax, 0x96(%0)\n\t"
        "mov %%gs, %%ax\n\t"
        "movw %%ax, 0x98(%0)\n\t"
        "mov %%ss, %%ax\n\t"
        "movw %%ax, 0x9a(%0)\n\t"
        "mov %%cr0, %%rax\n\t"
        "movq %%rax, 0x9c(%0)\n\t"
        "mov %%cr2, %%rax\n\t"
        "movq %%rax, 0xa4(%0)\n\t"
        "mov %%cr3, %%rax\n\t"
        "movq %%rax, 0xac(%0)\n\t"
        "mov %%cr4, %%rax\n\t"
        "movq %%rax, 0xb4(%0)"
        :
        : "r"(regs)
        : "rax", "memory");
}

static inline bool check_interrupt_status(void)
{
    volatile uint64_t flags;
    
    asm volatile(
        "pushfq\n\t"
        "popq %0"
        : "=g"(flags)
        :
        : "memory"
    );
    
    return (flags & 0x200) ? true : false;
}

static void draw_frame(void) {
    if (!term || !term->fb) return;
    
    // Верхняя синяя полоса
    fb_fill_rect(term->fb, 0, 0, term->fb->width, 30, (color_t){170, 0, 0});
    
    // Белый текст в синей полосе
    uint32_t old_x = term->cursor_x;
    uint32_t old_y = term->cursor_y;
    color_t old_fg = term->fb->fg_color;
    color_t old_bg = term->fb->bg_color;
    
    fb_set_color(term->fb, COLOR_WHITE, (color_t){170, 0, 0});
    fb_set_cursor(term->fb, 10, 10);
    fb_print(term->fb, "ALK");
    
    // Восстанавливаем
    fb_set_color(term->fb, old_fg, old_bg);
    fb_set_cursor(term->fb, old_x, old_y);
}

static void rsod_print_left(const char* text, uint32_t x, uint32_t y) {
    if (!term || !text) return;
    
    uint32_t old_x = term->cursor_x;
    uint32_t old_y = term->cursor_y;
    
    fb_set_cursor(term->fb, x, y);
    fb_print(term->fb, text);
    
    fb_set_cursor(term->fb, old_x, old_y);
}

static void format_hex64(uint64_t value, char* buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    
    buffer[0] = '0';
    buffer[1] = 'x';
    
    for (int i = 0; i < 16; i++) {
        int nibble = (value >> (60 - i * 4)) & 0xF;
        buffer[i + 2] = hex_chars[nibble];
    }
    buffer[18] = '\0';
}

// Функция для форматирования 16-битного hex значения
static void format_hex16(uint16_t value, char* buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    
    buffer[0] = '0';
    buffer[1] = 'x';
    
    for (int i = 0; i < 4; i++) {
        int nibble = (value >> (12 - i * 4)) & 0xF;
        buffer[i + 2] = hex_chars[nibble];
    }
    buffer[6] = '\0';
}

static void rsod_clear_screen(void) {
    if (term && term->fb) {
        fb_clear(term->fb, (color_t){170, 0, 0}); // Темно-красный фон
    }
}

void rsod(const char *stopcode, const char *subsystem) {
    if (rsod_is_running == 1) {
        return;
    }

    if (check_interrupt_status()) {
        asm volatile ("cli");
    }

    rsod_is_running = 1;

    task_t* ctask = get_current_task();
    kill_all_tasks();

    rsod_clear_screen();

    draw_frame();

    fb_set_color(term->fb, COLOR_WHITE, (color_t){170, 0, 0});

    uint32_t start_y = 50;
    uint32_t margin_left = 20;
    char buffer[256];

    rsod_print_left("A problem has been detected and ALK has been shut down to prevent damage", 
                    margin_left, start_y);
    start_y += 30;

    rsod_print_left("to your computer. System will be rebooted after 5 seconds.", margin_left, start_y);
    start_y += 40;

    strcpy(buffer, "STOP: ");
    strcat(buffer, stopcode ? stopcode : "0x00000000");
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 30;
    
    // Подсистема
    if (subsystem) {
        strcpy(buffer, "Subsystem: ");
        strcat(buffer, subsystem);
        rsod_print_left(buffer, margin_left, start_y);
        start_y += 30;
    }
    
    start_y += 20;
    
    // Получаем регистры для отладки
    registers_state regs;
    get_registers(&regs);
    
    // Выводим ключевые регистры как в Windows XP
    char hex_buf[32];
    
    // Блок с адресами
    rsod_print_left("Technical information:", margin_left, start_y);
    start_y += 20;
    
    // STOP код с адресом
    format_hex64(regs.rip, hex_buf);
    strcpy(buffer, "*** STOP: ");
    strcat(buffer, stopcode ? stopcode : "0x00000000");
    strcat(buffer, " (");
    strcat(buffer, hex_buf);
    strcat(buffer, ")");
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    // Адрес ошибки (CR2 для page fault)
    if (subsystem && strstr(subsystem, "PAGE_FAULT")) {
        format_hex64(regs.cr2, hex_buf);
        strcpy(buffer, "*** Address ");
        strcat(buffer, hex_buf);
        strcat(buffer, " referenced at ");
        format_hex64(regs.rip, hex_buf);
        strcat(buffer, hex_buf);
        rsod_print_left(buffer, margin_left, start_y);
        start_y += 20;
    }
    
    // Дата и время сборки
    rsod_print_left("*** ALK Version 0.02", 
                    margin_left, start_y);
    start_y += 40;
    
    // Детальная информация о регистрах
    rsod_print_left("Register dump:", margin_left, start_y);
    start_y += 20;
    
    // Первая строка регистров
    format_hex64(regs.rax, hex_buf);
    strcpy(buffer, "RAX=");
    strcat(buffer, hex_buf);
    strcat(buffer, " RBX=");
    format_hex64(regs.rbx, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " RCX=");
    format_hex64(regs.rcx, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    // Вторая строка регистров
    format_hex64(regs.rdx, hex_buf);
    strcpy(buffer, "RDX=");
    strcat(buffer, hex_buf);
    strcat(buffer, " RSI=");
    format_hex64(regs.rsi, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " RDI=");
    format_hex64(regs.rdi, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    // Третья строка регистров
    format_hex64(regs.rbp, hex_buf);
    strcpy(buffer, "RBP=");
    strcat(buffer, hex_buf);
    strcat(buffer, " RSP=");
    format_hex64(regs.rsp, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " RIP=");
    format_hex64(regs.rip, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    // Четвертая строка регистров
    format_hex64(regs.r8, hex_buf);
    strcpy(buffer, "R8 =");
    strcat(buffer, hex_buf);
    strcat(buffer, " R9 =");
    format_hex64(regs.r9, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " R10=");
    format_hex64(regs.r10, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    // Пятая строка регистров
    format_hex64(regs.r11, hex_buf);
    strcpy(buffer, "R11=");
    strcat(buffer, hex_buf);
    strcat(buffer, " R12=");
    format_hex64(regs.r12, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " R13=");
    format_hex64(regs.r13, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    // Шестая строка регистров
    format_hex64(regs.r14, hex_buf);
    strcpy(buffer, "R14=");
    strcat(buffer, hex_buf);
    strcat(buffer, " R15=");
    format_hex64(regs.r15, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 30;
    
    // Регистры сегментов
    format_hex16(regs.cs, hex_buf);
    strcpy(buffer, "CS =");
    strcat(buffer, hex_buf);
    strcat(buffer, " DS=");
    format_hex16(regs.ds, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " ES=");
    format_hex16(regs.es, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    strcpy(buffer, "FS =");
    format_hex16(regs.fs, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " GS=");
    format_hex16(regs.gs, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " SS=");
    format_hex16(regs.ss, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 30;
    
    // Контрольные регистры
    format_hex64(regs.cr0, hex_buf);
    strcpy(buffer, "CR0=");
    strcat(buffer, hex_buf);
    strcat(buffer, " CR2=");
    format_hex64(regs.cr2, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 20;
    
    format_hex64(regs.cr3, hex_buf);
    strcpy(buffer, "CR3=");
    strcat(buffer, hex_buf);
    strcat(buffer, " CR4=");
    format_hex64(regs.cr4, hex_buf);
    strcat(buffer, hex_buf);
    strcat(buffer, " EFL=");
    format_hex64(regs.rflags, hex_buf);
    strcat(buffer, hex_buf);
    rsod_print_left(buffer, margin_left, start_y);
    start_y += 40;
    
    pc_speaker_play(200);
    
    wait(5);
    acpi_reboot();

    while (1) {
        asm volatile ("pause");
    }
}