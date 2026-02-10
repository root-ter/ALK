/* DO NOT TOUCH POWER FUNCTIONS!!!
   This functions very unstable and
   one edited line can broke power!
*/

#include "acpi.h"
#include "../../libc/string.h"
#include "../../base/mb2/mb2.h"
#include "../io/io.h"
#include "../../base/term/term.h"
#include "../../base/sched/sched.h"
#include "aml.h"
#include <stddef.h>

// Глобальный контекст
static acpi_context_t g_acpi_ctx;
extern term_t* term;
static aml_context_t g_aml_ctx;
static bool aml_initialized = false;

// ==================== УНИВЕРСАЛЬНЫЙ МОНИТОР КНОПКИ ПИТАНИЯ ====================

// Флаги для монитора
static volatile bool power_button_pressed = false;
static bool monitor_initialized = false;
static uint16_t pm1a_event_port = 0;
static uint8_t power_button_bit = 8; // Стандартный бит для кнопки питания ACPI

static bool init_aml_power_detection(void) {
    if (aml_initialized) return true;
    
    if (!aml_init_context(&g_aml_ctx, &g_acpi_ctx)) {
        return false;
    }
    
    aml_initialized = true;
    return true;
}

// Детекция эмулятора (только проверка, без выключения)
static const char* detect_emulator(void) {
    // Проверка через BIOS Data Area
    uint8_t bios_flag = inb(0x40);
    
    if (bios_flag == 0xE9) return "QEMU";
    if (bios_flag == 0xFC) return "Bochs";
    
    // Проверка через порты эмуляторов (чтение, а не запись!)
    uint16_t qemu_test = inw(0x604);
    uint16_t bochs_test = inw(0xB004);
    uint16_t vbox_test = inw(0x4004);
    
    if (qemu_test == 0x1234) return "QEMU";
    if (bochs_test == 0x1234) return "Bochs"; 
    if (vbox_test == 0x1234) return "VirtualBox";
    
    return "Unknown/Physical";
}

// Инициализация монитора
static bool init_power_monitor(void) {
    term_printf(term, "[PWRMON] Initializing power button monitor\n");
    
    // Инициализируем AML парсер
    init_aml_power_detection();
    
    // Получаем информацию о кнопке питания из AML
    power_button_info_t pwr_info;
    bool aml_found = aml_get_power_button_info(&pwr_info);
    
    if (aml_found) {    
        // Настраиваем мониторинг на основе AML информации
        if (pwr_info.power_button_region == 0x81) { // IO Space
            // Для IO Space можно настроить прямое чтение порта
            pm1a_event_port = pwr_info.power_button_offset;
            power_button_bit = pwr_info.power_button_bit;
            
            return true;
        }
    }
    
    // Fallback к стандартному ACPI методу
    const char* emulator = detect_emulator();
    
    if (g_acpi_ctx.fadt && g_acpi_ctx.acpi_enabled) {
        pm1a_event_port = g_acpi_ctx.fadt->pm1a_evt_blk;
        
        if (pm1a_event_port) {
            
            // Включаем событие кнопки питания
            uint16_t enable_port = pm1a_event_port + 2; // Enable register
            uint16_t current_enable = inw(enable_port);
            outw(enable_port, current_enable | (1 << power_button_bit));
            
            return true;
        }
    }
    
    return false;
}

static bool check_aml_power_button(void) {
    if (!aml_initialized || !g_aml_ctx.pwr_info.found) {
        return false;
    }
    
    power_button_info_t* info = &g_aml_ctx.pwr_info;
    
    // Проверяем в зависимости от региона
    switch (info->power_button_region) {
        case 0x81: // IO Space
            if (info->power_button_offset > 0) {
                uint8_t value = inb(info->power_button_offset);
                if (value & (1 << info->power_button_bit)) {
                    // Очищаем бит
                    outb(info->power_button_offset, value & ~(1 << info->power_button_bit));
                    
                    return true;
                }
            }
            break;
            
        case 0x80: // Memory Space
            if (info->power_button_offset > 0) {
                uint8_t* mem_addr = (uint8_t*)(uintptr_t)info->power_button_offset;
                uint8_t value = *mem_addr;
                
                if (value & (1 << info->power_button_bit)) {
                    // Очищаем бит
                    *mem_addr = value & ~(1 << info->power_button_bit);
                    
                    return true;
                }
            }
            break;
            
        default:
            // EC Space - сложнее, требует чтения через EC контроллер
            break;
    }
    
    return false;
}

// Проверка через ACPI (основной метод)
static bool check_acpi_power_button(void) {
    if (!pm1a_event_port) return false;
    
    uint16_t status = inw(pm1a_event_port);
    if (status & (1 << power_button_bit)) {
        power_button_pressed = true;
        
        // Очищаем событие
        outw(pm1a_event_port, (1 << power_button_bit));
        return true;
    }
    
    return false;
}

// Проверка специальных регистров эмуляторов (только чтение!)
static bool check_emulator_power_button(void) {
    // Только читаем статусные регистры, не пишем команды выключения!
    
    // QEMU - проверяем порт управления (только чтение)
    uint16_t qemu_status = inw(0x604);
    // В QEMU нет статуса кнопки питания, только управляющий порт
    
    // Bochs - проверяем порт отладки
    uint16_t bochs_status = inw(0xB004);
    
    // VirtualBox
    uint16_t vbox_status = inw(0x4004);
    
    // Если у эмуляторов есть регистры статуса кнопки, проверяем их здесь
    // Но обычно эмуляторы полагаются на ACPI для кнопки питания
    
    return false;
}

// Главная функция монитора (без аргументов)
void acpi_monitor_power_button(void) {
    
    // Инициализация
    if (!monitor_initialized) {
        monitor_initialized = init_power_monitor();
        if (!monitor_initialized) {
        }
    }
    
    // Основной цикл мониторинга
    while (1) {
        bool button_pressed = false;
        
        // Метод 1: AML (если найден)
        if (aml_initialized && g_aml_ctx.pwr_info.found) {
            button_pressed = check_aml_power_button();
        }
        
        // Метод 2: Стандартный ACPI
        if (!button_pressed && pm1a_event_port) {
            button_pressed = check_acpi_power_button();
        }
        
        // Метод 3: Эмуляторы (только проверка статуса)
        if (!button_pressed) {
            button_pressed = check_emulator_power_button();
        }
        
        // Если кнопка нажата, обрабатываем
        if (button_pressed) {       
            // Таймер обратного отсчёта с возможностью отмены
            bool cancelled = false;
            for (int i = 3; i > 0 && !cancelled; i--) {
                
                // Ждём 1 секунду с проверкой отмены
                for (int j = 0; j < 100; j++) {
                    // Проверяем не нажали ли кнопку снова (отмена)
                    bool new_press = false;
                    
                    if (aml_initialized && g_aml_ctx.pwr_info.found) {
                        new_press = check_aml_power_button();
                    }
                    
                    if (!new_press && pm1a_event_port) {
                        new_press = check_acpi_power_button();
                    }
                    
                    if (new_press) {
                        cancelled = true;
                        break;
                    }
                    
                    // Задержка 10мс
                    for (volatile int k = 0; k < 1000; k++) {
                        asm volatile("pause");
                    }
                }
            }
            
            // Если не отменили, выключаем
            if (!cancelled) {
                acpi_shutdown();
                
                // На всякий случай
                while (1) asm volatile("hlt");
            }
        }
        
        // Задержка перед следующей проверкой (100мс)
        for (volatile int i = 0; i < 10000; i++) {
            asm volatile("pause");
        }
    }
}

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

// Проверка контрольной суммы
bool acpi_checksum_valid(const uint8_t* data, size_t length) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += data[i];
    }
    return sum == 0;
}

// Поиск таблицы в RSDT/XSDT
static void* find_table_in_rsdt(const char* signature, rsdt_t* rsdt, bool xsdt) {
    if (!rsdt || !signature) return NULL;
    
    // Количество записей
    size_t entries = (rsdt->header.length - sizeof(acpi_sdt_header_t));
    if (xsdt) {
        entries /= sizeof(uint64_t);
    } else {
        entries /= sizeof(uint32_t);
    }
    
    for (size_t i = 0; i < entries; i++) {
        uintptr_t table_addr;
        
        if (xsdt) {
            uint64_t* entries = (uint64_t*)(rsdt + 1);
            table_addr = (uintptr_t)entries[i];
        } else {
            uint32_t* entries = (uint32_t*)(rsdt + 1);
            table_addr = (uintptr_t)entries[i];
        }
        
        if (!table_addr) continue;
        
        acpi_sdt_header_t* header = (acpi_sdt_header_t*)table_addr;
        
        // Проверяем сигнатуру
        if (memcmp(header->signature, signature, 4) == 0) {
            // Проверяем контрольную сумму
            if (acpi_checksum_valid((uint8_t*)header, header->length)) {
                return header;
            }
        }
    }
    
    return NULL;
}

// Парсинг MADT
static void parse_madt(madt_t* madt) {
    if (!madt) return;
    
    g_acpi_ctx.local_apic_addr = madt->local_apic_address;
    
    // Проходим по записям MADT
    uint8_t* ptr = (uint8_t*)(madt + 1);
    uint8_t* end = (uint8_t*)madt + madt->header.length;
    
    while (ptr < end) {
        madt_header_t* header = (madt_header_t*)ptr;
        
        switch (header->type) {
            case MADT_TYPE_LOCAL_APIC: {
                madt_local_apic_t* lapic = (madt_local_apic_t*)ptr;
                if (lapic->flags & 1) { // Processor enabled
                    term_printf(term, "[ACPI] CPU %d: APIC ID %d\n",
                               lapic->processor_id, lapic->apic_id);
                }
                break;
            }
            
            case MADT_TYPE_IO_APIC: {
                madt_io_apic_t* ioapic = (madt_io_apic_t*)ptr;
                g_acpi_ctx.io_apic_addr = ioapic->io_apic_address;
                g_acpi_ctx.gsi_base = ioapic->global_system_interrupt_base;
                term_printf(term, "[ACPI] IOAPIC: ID %d, Addr 0x%x, GSI %d\n",
                           ioapic->io_apic_id, ioapic->io_apic_address,
                           ioapic->global_system_interrupt_base);
                break;
            }
            
            case MADT_TYPE_INTERRUPT_SOURCE: {
                madt_interrupt_source_t* isrc = (madt_interrupt_source_t*)ptr;
                term_printf(term, "[ACPI] IRQ Source: Bus %d, IRQ %d -> GSI %d\n",
                           isrc->bus, isrc->source, isrc->interrupt);
                break;
            }
        }
        
        ptr += header->length;
    }
}

// Включение ACPI через FADT
static void enable_acpi(fadt_t* fadt) {
    if (!fadt) return;
    
    // Проверяем, поддерживает ли система ACPI
    if (!(fadt->flags & (1 << 4))) { // HW_REDUCED_ACPI
        // Отправляем команду включения ACPI
        if (fadt->smi_cmd && fadt->acpi_enable) {
            outb(fadt->smi_cmd, fadt->acpi_enable);
            
            // Ждём установки бита SCI_EN
            uint16_t pm1_cnt_port = fadt->pm1a_cnt_blk;
            if (pm1_cnt_port) {
                uint16_t pm1_cnt = inw(pm1_cnt_port);
                while (!(pm1_cnt & (1 << 0))) { // SCI_EN bit
                    asm volatile("pause");
                    pm1_cnt = inw(pm1_cnt_port);
                }
            }
            
            term_printf(term, "[ACPI] SCI enabled\n");
        }
    }
    
    g_acpi_ctx.acpi_enabled = true;
}

// ==================== ОСНОВНЫЕ ФУНКЦИИ ====================

// Инициализация ACPI
bool acpi_init(void) {
    term_printf(term, "[ACPI] Initializing...\n");
    
    // Очищаем контекст
    memset(&g_acpi_ctx, 0, sizeof(acpi_context_t));
    
    // Получаем RSDP из Multiboot2
    uint64_t rsdp_addr = get_rsdp_address();
    if (!rsdp_addr) {
        term_printf(term, "[ACPI] RSDP not found via Multiboot2\n");
        return false;
    }
    
    g_acpi_ctx.rsdp = (rsdp_t*)(uintptr_t)rsdp_addr;
    
    // Проверяем RSDP
    if (memcmp(g_acpi_ctx.rsdp->signature, "RSD PTR ", 8) != 0) {
        term_printf(term, "[ACPI] Invalid RSDP signature\n");
        return false;
    }
    
    if (!acpi_checksum_valid((uint8_t*)g_acpi_ctx.rsdp, 20)) {
        term_printf(term, "[ACPI] RSDP checksum invalid\n");
        return false;
    }
    
    term_printf(term, "[ACPI] RSDP v%d found at 0x%llx\n",
               g_acpi_ctx.rsdp->revision, rsdp_addr);
    
    // Определяем, используем ли XSDT (ACPI 2.0+)
    g_acpi_ctx.xsdt_present = (g_acpi_ctx.rsdp->revision >= 2 && 
                               g_acpi_ctx.rsdp->xsdt_address != 0);
    
    // Загружаем RSDT/XSDT
    if (g_acpi_ctx.xsdt_present) {
        g_acpi_ctx.rsdt = (rsdt_t*)(uintptr_t)g_acpi_ctx.rsdp->xsdt_address;
        term_printf(term, "[ACPI] Using XSDT at 0x%llx\n", 
                   g_acpi_ctx.rsdp->xsdt_address);
    } else {
        g_acpi_ctx.rsdt = (rsdt_t*)(uintptr_t)g_acpi_ctx.rsdp->rsdt_address;
        term_printf(term, "[ACPI] Using RSDT at 0x%x\n",
                   g_acpi_ctx.rsdp->rsdt_address);
    }
    
    if (!g_acpi_ctx.rsdt) {
        term_printf(term, "[ACPI] No RSDT/XSDT found\n");
        return false;
    }
    
    // Проверяем RSDT/XSDT
    if (!acpi_checksum_valid((uint8_t*)g_acpi_ctx.rsdt, g_acpi_ctx.rsdt->header.length)) {
        term_printf(term, "[ACPI] RSDT/XSDT checksum invalid\n");
        return false;
    }
    
    // Ищем основные таблицы
    g_acpi_ctx.fadt = (fadt_t*)acpi_find_table("FACP");
    g_acpi_ctx.madt = (madt_t*)acpi_find_table("APIC");
    g_acpi_ctx.mcfg = (mcfg_t*)acpi_find_table("MCFG");
    
    // DSDT находится внутри FADT
    if (g_acpi_ctx.fadt) {
        if (g_acpi_ctx.fadt->header.revision >= 2 && g_acpi_ctx.fadt->x_dsdt) {
            g_acpi_ctx.dsdt = (ssdt_t*)(uintptr_t)g_acpi_ctx.fadt->x_dsdt;
        } else {
            g_acpi_ctx.dsdt = (ssdt_t*)(uintptr_t)g_acpi_ctx.fadt->dsdt;
        }
    }
    
    // Ищем SSDT таблицы
    const char* signatures[] = {"SSDT", "SSD2", "SSD3", "SSD4", "SSD5"};
    for (int i = 0; i < 5; i++) {
        ssdt_t* ssdt = (ssdt_t*)acpi_find_table(signatures[i]);
        if (ssdt && g_acpi_ctx.ssdt_count < 16) {
            g_acpi_ctx.ssdts[g_acpi_ctx.ssdt_count++] = ssdt;
        }
    }
    
    // Парсим MADT если есть
    if (g_acpi_ctx.madt) {
        parse_madt(g_acpi_ctx.madt);
    }
    
    // Включаем ACPI
    if (g_acpi_ctx.fadt) {
        enable_acpi(g_acpi_ctx.fadt);
    }
    
    term_printf(term, "[ACPI] Initialized successfully\n");
    term_printf(term, "[ACPI] Tables: %s, FADT: %s, MADT: %s, MCFG: %s\n",
               g_acpi_ctx.xsdt_present ? "XSDT" : "RSDT",
               g_acpi_ctx.fadt ? "yes" : "no",
               g_acpi_ctx.madt ? "yes" : "no",
               g_acpi_ctx.mcfg ? "yes" : "no");
    
    return true;
}

// Найти таблицу по сигнатуре
void* acpi_find_table(const char* signature) {
    if (!g_acpi_ctx.rsdt || !signature) return NULL;
    
    // Прямой поиск в RSDT/XSDT
    void* table = find_table_in_rsdt(signature, g_acpi_ctx.rsdt, 
                                     g_acpi_ctx.xsdt_present);
    
    if (!table) {
        // Проверяем основные таблицы вручную
        for (int i = 0; i < g_acpi_ctx.ssdt_count; i++) {
            acpi_sdt_header_t* header = &g_acpi_ctx.ssdts[i]->header;
            if (memcmp(header->signature, signature, 4) == 0) {
                return header;
            }
        }
    }
    
    return table;
}

// Получить контекст
acpi_context_t* acpi_get_context(void) {
    return &g_acpi_ctx;
}

// Получить количество CPU
int acpi_get_cpu_count(void) {
    if (!g_acpi_ctx.madt) return 1;
    
    int count = 0;
    uint8_t* ptr = (uint8_t*)(g_acpi_ctx.madt + 1);
    uint8_t* end = (uint8_t*)g_acpi_ctx.madt + g_acpi_ctx.madt->header.length;
    
    while (ptr < end) {
        madt_header_t* header = (madt_header_t*)ptr;
        
        if (header->type == MADT_TYPE_LOCAL_APIC) {
            madt_local_apic_t* lapic = (madt_local_apic_t*)ptr;
            if (lapic->flags & 1) { // Enabled
                count++;
            }
        }
        
        ptr += header->length;
    }
    
    return count > 0 ? count : 1;
}

// Получить адрес IOAPIC
uint32_t acpi_get_ioapic_address(void) {
    return g_acpi_ctx.io_apic_addr;
}

// Получить адрес Local APIC
uint32_t acpi_get_local_apic_address(void) {
    return g_acpi_ctx.local_apic_addr;
}

// Получить MCFG
mcfg_t* acpi_get_mcfg(void) {
    return g_acpi_ctx.mcfg;
}

// ==================== POWER MANAGEMENT ====================

// Перезагрузка через ACPI
void acpi_reboot(void) {
    kill_all_tasks();

    if (!g_acpi_ctx.fadt) {
        // Fallback к 8042
        outb(0x64, 0xFE);
        return;
    }
    
    // Проверяем наличие reset регистра
    if (g_acpi_ctx.fadt->reset_reg[0] == 0x01) { // System I/O
        uint16_t port = *(uint16_t*)&g_acpi_ctx.fadt->reset_reg[4];
        uint8_t value = g_acpi_ctx.fadt->reset_value;
        
        outb(port, value);
        term_printf(term, "[ACPI] Reset via I/O port 0x%x\n", port);
    } else if (g_acpi_ctx.fadt->reset_reg[0] == 0x02) { // System Memory
        uint64_t addr = *(uint64_t*)&g_acpi_ctx.fadt->reset_reg[4];
        uint8_t value = g_acpi_ctx.fadt->reset_value;
        
        *(uint8_t*)(uintptr_t)addr = value;
        term_printf(term, "[ACPI] Reset via memory 0x%llx\n", addr);
    } else {
        // Fallback
        outb(0x64, 0xFE);
    }
    
    while(1);
}

// Выключение через ACPI (S5 state)
void acpi_shutdown(void) {
    if (!g_acpi_ctx.fadt || !g_acpi_ctx.acpi_enabled) {
        term_printf(term, "[ACPI] Shutdown not supported\n");
        return;
    }
    
    // Входим в состояние S5 (soft off)
    uint16_t pm1a_port = g_acpi_ctx.fadt->pm1a_cnt_blk;
    if (!pm1a_port) {
        term_printf(term, "[ACPI] No PM1a control port\n");
        return;
    }
    
    // SLP_TYP для S5 = 7, SLP_EN = 1
    uint16_t value = (7 << 10) | (1 << 13);
    outw(pm1a_port, value);
    
    term_printf(term, "[ACPI] Shutting down...\n");
    
    // Должны выключиться
    while(1);
}

// Дамп информации
void acpi_dump_info(void) {
    if (!term) return;
    
    term_printf(term, "=== ACPI Information ===\n");
    term_printf(term, "RSDP: 0x%llx, Revision: %d, %s\n",
               (uint64_t)(uintptr_t)g_acpi_ctx.rsdp,
               g_acpi_ctx.rsdp->revision,
               g_acpi_ctx.xsdt_present ? "XSDT" : "RSDT");
    
    if (g_acpi_ctx.fadt) {
        term_printf(term, "FADT: 0x%llx, SCI IRQ: %d\n",
                   (uint64_t)(uintptr_t)g_acpi_ctx.fadt,
                   g_acpi_ctx.fadt->sci_int);
    }
    
    if (g_acpi_ctx.madt) {
        term_printf(term, "MADT: 0x%llx, CPUs: %d\n",
                   (uint64_t)(uintptr_t)g_acpi_ctx.madt,
                   acpi_get_cpu_count());
        term_printf(term, "  Local APIC: 0x%x\n", g_acpi_ctx.local_apic_addr);
        term_printf(term, "  IO APIC: 0x%x\n", g_acpi_ctx.io_apic_addr);
    }
    
    if (g_acpi_ctx.mcfg) {
        term_printf(term, "MCFG: 0x%llx\n", 
                   (uint64_t)(uintptr_t)g_acpi_ctx.mcfg);
    }
    
    if (g_acpi_ctx.dsdt) {
        term_printf(term, "DSDT: 0x%llx, Length: %d\n",
                   (uint64_t)(uintptr_t)g_acpi_ctx.dsdt,
                   g_acpi_ctx.dsdt->header.length);
    }
    
    term_printf(term, "SSDTs: %d\n", g_acpi_ctx.ssdt_count);
}