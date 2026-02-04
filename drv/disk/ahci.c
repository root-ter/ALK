#include "ahci.h"
#include "../io/io.h"
#include "../../libc/string.h"
#include "../../base/mem/mem.h"
#include "../../base/term/term.h"
#include "../../base/int/idt.h"
#include "../pic/pic.h"
#include <stddef.h>

// ==================== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ====================
static ahci_controller_t g_ahci_ctrl;
extern term_t* term;
extern void isr_ahci();

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

// Обёртки для регистров
uint32_t ahci_read_reg(uintptr_t base, uint32_t offset) {
    return mmio_read32((volatile void*)(base + offset));
}

void ahci_write_reg(uintptr_t base, uint32_t offset, uint32_t value) {
    mmio_write32((volatile void*)(base + offset), value);
}

uint32_t ahci_read_port(ahci_port_t* port, uint32_t offset) {
    return ahci_read_reg(g_ahci_ctrl.abar, 0x100 + port->port_num * 0x80 + offset);
}

void ahci_write_port(ahci_port_t* port, uint32_t offset, uint32_t value) {
    ahci_write_reg(g_ahci_ctrl.abar, 0x100 + port->port_num * 0x80 + offset, value);
}

// Выделение выровненной памяти
static void* ahci_alloc_aligned(size_t size, size_t align) {
    void* ptr = malloc(size + align - 1 + sizeof(void*));
    if (!ptr) return NULL;
    
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned = (addr + sizeof(void*) + align - 1) & ~(align - 1);
    
    // Сохраняем оригинальный указатель перед выровненным блоком
    *((void**)(aligned - sizeof(void*))) = ptr;
    
    return (void*)aligned;
}

static void ahci_free_aligned(void* aligned) {
    if (!aligned) return;
    void* original = *((void**)((uintptr_t)aligned - sizeof(void*)));
    free(original);
}

// Ожидание бита
static bool ahci_wait_bit(volatile uint32_t* reg, uint32_t bit, bool state, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        bool current = (mmio_read32(reg) & bit) != 0;
        if (current == state) return true;
        for (volatile int j = 0; j < 1000; j++); // ~1ms delay
    }
    return false;
}

// ==================== ОСНОВНЫЕ ФУНКЦИИ ====================

// Инициализация порта
void ahci_port_rebase(ahci_port_t* port) {
    // 1. Останавливаем порт
    uint32_t cmd = ahci_read_port(port, PORT_CMD);
    if (cmd & CMD_ST) {
        cmd &= ~CMD_ST;
        ahci_write_port(port, PORT_CMD, cmd);
        ahci_wait_bit((volatile uint32_t*)port->cmd, CMD_CR, false, 500); // 500ms timeout
    }
    
    // 2. Выделяем память для Command List (1KB, выровненная по 1KB)
    port->cl = (hba_cmd_header_t*)ahci_alloc_aligned(1024, 1024);
    memset(port->cl, 0, 1024);
    
    // 3. Выделяем память для FIS (256B, выровненная по 256B)
    port->fis = ahci_alloc_aligned(256, 256);
    memset(port->fis, 0, 256);
    
    // 4. Устанавливаем адреса в регистры порта
    uintptr_t cl_phys = (uintptr_t)port->cl;
    uintptr_t fis_phys = (uintptr_t)port->fis;
    
    ahci_write_port(port, PORT_CLB, (uint32_t)(cl_phys & 0xFFFFFFFF));
    ahci_write_port(port, PORT_CLBU, (uint32_t)(cl_phys >> 32));
    ahci_write_port(port, PORT_FB, (uint32_t)(fis_phys & 0xFFFFFFFF));
    ahci_write_port(port, PORT_FBU, (uint32_t)(fis_phys >> 32));
    
    // 5. Выделяем Command Tables для каждого слота
    uint32_t cmd_slots = ((*g_ahci_ctrl.cap >> 8) & 0x1F) + 1;
    for (uint32_t i = 0; i < cmd_slots; i++) {
        // Command Table (128B + PRDs) выровненная по 128B
        port->ct[i] = (hba_cmd_table_t*)ahci_alloc_aligned(sizeof(hba_cmd_table_t) + 16 * sizeof(hba_prd_t), 128);
        memset(port->ct[i], 0, sizeof(hba_cmd_table_t) + 16 * sizeof(hba_prd_t));
        
        uintptr_t ct_phys = (uintptr_t)port->ct[i];
        port->cl[i].ctba = (uint32_t)(ct_phys & 0xFFFFFFFF);
        port->cl[i].ctbau = (uint32_t)(ct_phys >> 32);
    }
}

// Запуск порта
void ahci_start_cmd(ahci_port_t* port) {
    // Включаем FIS Receive
    uint32_t cmd = ahci_read_port(port, PORT_CMD);
    cmd |= CMD_FRE;
    ahci_write_port(port, PORT_CMD, cmd);
    
    // Ждем пока FIS Receive начнет работать
    ahci_wait_bit((volatile uint32_t*)port->cmd, CMD_FR, true, 500);
    
    // Запускаем Command List
    cmd = ahci_read_port(port, PORT_CMD);
    cmd |= CMD_ST;
    ahci_write_port(port, PORT_CMD, cmd);
    
    // Ждем запуска
    ahci_wait_bit((volatile uint32_t*)port->cmd, CMD_CR, true, 500);
}

// Остановка порта
void ahci_stop_cmd(ahci_port_t* port) {
    // Останавливаем Command List
    uint32_t cmd = ahci_read_port(port, PORT_CMD);
    cmd &= ~CMD_ST;
    ahci_write_port(port, PORT_CMD, cmd);
    
    // Ждем остановки
    ahci_wait_bit((volatile uint32_t*)port->cmd, CMD_CR, false, 500);
    
    // Останавливаем FIS Receive
    cmd = ahci_read_port(port, PORT_CMD);
    cmd &= ~CMD_FRE;
    ahci_write_port(port, PORT_CMD, cmd);
    
    // Ждем остановки FIS
    ahci_wait_bit((volatile uint32_t*)port->cmd, CMD_FR, false, 500);
}

// Обнаружение устройства на порту
bool ahci_probe_port(ahci_port_t* port) {
    uint32_t ssts = ahci_read_port(port, PORT_SSTS);
    uint32_t det = ssts & 0x0F;
    uint32_t ipm = (ssts >> 8) & 0x0F;
    
    term_printf(term, "[AHCI] Port %d: DET=0x%X, IPM=0x%X, SIG=0x%X\n", 
                port->port_num, det, ipm, ahci_read_port(port, PORT_SIG));
    
    // Проверяем состояние подключения
    if (det != 3 || ipm != 1) {
        term_printf(term, "[AHCI] Port %d: No device connected\n", port->port_num);
        return false;
    }
    
    // Проверяем сигнатуру
    uint32_t sig = ahci_read_port(port, PORT_SIG);
    if (sig == 0xFFFFFFFF || sig == 0x00000000) {
        term_printf(term, "[AHCI] Port %d: Invalid signature\n", port->port_num);
        return false;
    }
    
    port->active = true;
    port->has_device = true;
    
    // Определяем тип устройства по сигнатуре
    switch (sig) {
        case 0xEB140101: // SATA
            term_printf(term, "[AHCI] Port %d: SATA device\n", port->port_num);
            port->sata_speed = (ssts >> 4) & 0x0F;
            term_printf(term, "[AHCI] Port %d: SATA speed Gen%d\n", 
                       port->port_num, port->sata_speed);
            break;
        case 0xC33C0101: // SATAPI
            term_printf(term, "[AHCI] Port %d: SATAPI device\n", port->port_num);
            break;
        case 0x96690101: // Enclosure Management Bridge
            term_printf(term, "[AHCI] Port %d: SEMB device\n", port->port_num);
            break;
        case 0x00000101: // Port Multiplier
            term_printf(term, "[AHCI] Port %d: Port Multiplier\n", port->port_num);
            break;
        default:
            term_printf(term, "[AHCI] Port %d: Unknown device (SIG=0x%08X)\n", 
                       port->port_num, sig);
            break;
    }
    
    return true;
}

// IDENTIFY DEVICE
bool ahci_identify_device(ahci_port_t* port) {
    // Подготовка Command Header
    hba_cmd_header_t* cmdheader = &port->cl[0];
    memset(cmdheader, 0, sizeof(hba_cmd_header_t));
    
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t); // 5 DWORDs
    cmdheader->w = 0; // Read
    cmdheader->prdtl = 1; // 1 PRD
    
    // Подготовка Command Table
    hba_cmd_table_t* cmdtbl = port->ct[0];
    memset(cmdtbl, 0, sizeof(hba_cmd_table_t));
    
    // Настраиваем PRD
    uint16_t* identify_buffer = (uint16_t*)malloc(512);
    if (!identify_buffer) return false;
    
    cmdtbl->prdt[0].dba = (uint32_t)(uintptr_t)identify_buffer;
    cmdtbl->prdt[0].dbau = 0;
    cmdtbl->prdt[0].dbc = 511; // 512 bytes - 1
    cmdtbl->prdt[0].i = 1;
    
    // Создаем FIS
    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)cmdtbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    
    fis->type = FIS_TYPE_REG_H2D;
    fis->c = 1; // Command
    fis->command = ATA_CMD_IDENTIFY;
    fis->device = 0xA0; // LBA mode, Device 0
    fis->count = 1;
    fis->feature_low = 0;
    fis->feature_high = 0;
    fis->lba0 = 0;
    fis->lba1 = 0;
    fis->lba2 = 0;
    fis->lba3 = 0;
    fis->lba4 = 0;
    fis->lba5 = 0;
    fis->control = 0;
    
    // Очищаем прерывания
    ahci_write_port(port, PORT_IS, 0xFFFFFFFF);
    
    // Запускаем команду
    ahci_write_port(port, PORT_CI, 1 << 0);
    
    // Ждем завершения через прерывание или поллинг
    bool success = ahci_wait_irq(port, 0, 5000);
    
    if (success) {
        // Парсим IDENTIFY данные
        // Word 60-61: Total LBA28 sectors
        // Word 100-103: Total LBA48 sectors
        // Word 106: Physical sector size
        
        uint32_t lba28 = identify_buffer[60] | (identify_buffer[61] << 16);
        uint64_t lba48 = 0;
        
        // Проверяем поддержку LBA48
        if (identify_buffer[83] & (1 << 10)) {
            lba48 = (uint64_t)identify_buffer[100] |
                   ((uint64_t)identify_buffer[101] << 16) |
                   ((uint64_t)identify_buffer[102] << 32) |
                   ((uint64_t)identify_buffer[103] << 48);
            port->lba48 = true;
        }
        
        port->sectors = (lba48 > 0) ? lba48 : lba28;
        
        // Определяем размер сектора
        if (identify_buffer[106] & (1 << 14)) {
            // Word 117-118: Physical sector size
            port->sector_size = 1 << (identify_buffer[117] & 0x0F);
            if (port->sector_size == 1) port->sector_size = 512;
        } else {
            port->sector_size = 512;
        }
        
        term_printf(term, "[AHCI] Port %d: Sectors: %llu, Sector Size: %u, LBA48: %s\n",
                   port->port_num, port->sectors, port->sector_size,
                   port->lba48 ? "Yes" : "No");
    }
    
    free(identify_buffer);
    return success;
}

// Включение прерываний порта
void ahci_enable_interrupts(ahci_port_t* port) {
    // Включаем все прерывания
    ahci_write_port(port, PORT_IE, 0xFFFFFFFF);
    // Очищаем статус
    ahci_write_port(port, PORT_IS, 0xFFFFFFFF);
    
    // Включаем прерывания в HBA
    uint32_t ghc = mmio_read32(g_ahci_ctrl.ghc);
    ghc |= (1 << 1); // Enable interrupts
    mmio_write32(g_ahci_ctrl.ghc, ghc);
}

// Отключение прерываний порта
void ahci_disable_interrupts(ahci_port_t* port) {
    ahci_write_port(port, PORT_IE, 0);
    ahci_write_port(port, PORT_IS, 0xFFFFFFFF);
}

// Ожидание прерывания для слота
bool ahci_wait_irq(ahci_port_t* port, uint32_t slot, uint32_t timeout_ms) {
    port->irq_pending = false;
    
    for (uint32_t i = 0; i < timeout_ms; i++) {
        // Проверяем флаг прерывания
        if (port->irq_pending) {
            port->irq_pending = false;
            
            // Читаем статус прерываний
            uint32_t is = ahci_read_port(port, PORT_IS);
            
            // Проверяем завершение команды
            if (is & (1 << 30)) { // Task File Error
                term_printf(term, "[AHCI] Port %d: Task File Error (IS=0x%08X)\n", 
                           port->port_num, is);
                ahci_write_port(port, PORT_IS, is); // Clear
                return false;
            }
            
            if (is & (1 << 0)) { // Device to Host Register FIS
                // Проверяем бит завершения в регистре CI
                uint32_t ci = ahci_read_port(port, PORT_CI);
                if (!(ci & (1 << slot))) {
                    // Команда завершена
                    ahci_write_port(port, PORT_IS, is); // Clear
                    return true;
                }
            }
            
            // Очищаем обработанные прерывания
            ahci_write_port(port, PORT_IS, is);
        }
        
        // Короткая задержка
        for (volatile int j = 0; j < 1000; j++);
    }
    
    // Timeout - проверяем через polling
    uint32_t ci = ahci_read_port(port, PORT_CI);
    if (!(ci & (1 << slot))) {
        return true;
    }
    
    term_printf(term, "[AHCI] Port %d: Command timeout (CI=0x%08X)\n", 
               port->port_num, ci);
    return false;
}

// Чтение секторов
bool ahci_read(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer) {
    if (!port || !port->has_device || count == 0 || !buffer) {
        return false;
    }
    
    // Проверяем границы
    if (lba + count > port->sectors) {
        term_printf(term, "[AHCI] Read out of bounds: LBA=%llu, Count=%u, Max=%llu\n",
                   lba, count, port->sectors);
        return false;
    }
    
    // Находим свободный слот
    uint32_t ci = ahci_read_port(port, PORT_CI);
    uint32_t slot = 0;
    for (slot = 0; slot < g_ahci_ctrl.cmd_slots; slot++) {
        if (!(ci & (1 << slot))) break;
    }
    
    if (slot >= g_ahci_ctrl.cmd_slots) {
        term_printf(term, "[AHCI] No free command slots\n");
        return false;
    }
    
    // Настройка Command Header
    hba_cmd_header_t* cmdheader = &port->cl[slot];
    memset(cmdheader, 0, sizeof(hba_cmd_header_t));
    
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 0; // Read
    cmdheader->prdtl = (count * port->sector_size + 0x3FFFFF) / 0x400000; // PRD count
    
    if (cmdheader->prdtl > 65535) {
        term_printf(term, "[AHCI] Transfer too large\n");
        return false;
    }
    
    // Настройка Command Table
    hba_cmd_table_t* cmdtbl = port->ct[slot];
    memset(cmdtbl, 0, sizeof(hba_cmd_table_t) + cmdheader->prdtl * sizeof(hba_prd_t));
    
    // Настройка PRDs
    uint8_t* buf = (uint8_t*)buffer;
    uint32_t remaining = count * port->sector_size;
    
    for (uint32_t i = 0; i < cmdheader->prdtl; i++) {
        uint32_t chunk = remaining > 0x400000 ? 0x400000 : remaining;
        
        cmdtbl->prdt[i].dba = (uint32_t)(uintptr_t)buf;
        cmdtbl->prdt[i].dbau = (uint32_t)((uintptr_t)buf >> 32);
        cmdtbl->prdt[i].dbc = chunk - 1;
        cmdtbl->prdt[i].i = (i == cmdheader->prdtl - 1) ? 1 : 0;
        
        buf += chunk;
        remaining -= chunk;
    }
    
    // Настройка FIS
    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)cmdtbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    
    fis->type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->device = 0x40; // LBA mode
    
    if (port->lba48 && (lba > 0x0FFFFFFF || count > 256)) {
        // LBA48
        fis->command = ATA_CMD_READ_DMA_EXT;
        fis->lba0 = lba & 0xFF;
        fis->lba1 = (lba >> 8) & 0xFF;
        fis->lba2 = (lba >> 16) & 0xFF;
        fis->lba3 = (lba >> 24) & 0xFF;
        fis->lba4 = (lba >> 32) & 0xFF;
        fis->lba5 = (lba >> 40) & 0xFF;
        fis->count = count & 0xFFFF;
    } else {
        // LBA28
        fis->command = ATA_CMD_READ_DMA;
        fis->lba0 = lba & 0xFF;
        fis->lba1 = (lba >> 8) & 0xFF;
        fis->lba2 = (lba >> 16) & 0xFF;
        fis->lba3 = 0;
        fis->lba4 = 0;
        fis->lba5 = 0;
        fis->count = count & 0xFF;
        fis->device |= ((lba >> 24) & 0x0F);
    }
    
    // Очищаем прерывания
    ahci_write_port(port, PORT_IS, 0xFFFFFFFF);
    
    // Запускаем команду
    ahci_write_port(port, PORT_CI, 1 << slot);
    
    // Ждем завершения
    bool success = ahci_wait_irq(port, slot, 5000);
    
    if (!success) {
        term_printf(term, "[AHCI] Read failed at LBA %llu\n", lba);
    }
    
    return success;
}

// Запись секторов
bool ahci_write(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer) {
    // Аналогично read, но с w=1 и другой командой
    if (!port || !port->has_device || count == 0 || !buffer) {
        return false;
    }
    
    // Проверяем границы
    if (lba + count > port->sectors) {
        term_printf(term, "[AHCI] Write out of bounds: LBA=%llu, Count=%u, Max=%llu\n",
                   lba, count, port->sectors);
        return false;
    }
    
    // Находим свободный слот
    uint32_t ci = ahci_read_port(port, PORT_CI);
    uint32_t slot = 0;
    for (slot = 0; slot < g_ahci_ctrl.cmd_slots; slot++) {
        if (!(ci & (1 << slot))) break;
    }
    
    if (slot >= g_ahci_ctrl.cmd_slots) {
        term_printf(term, "[AHCI] No free command slots\n");
        return false;
    }
    
    // Настройка Command Header
    hba_cmd_header_t* cmdheader = &port->cl[slot];
    memset(cmdheader, 0, sizeof(hba_cmd_header_t));
    
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 1; // Write
    cmdheader->prdtl = (count * port->sector_size + 0x3FFFFF) / 0x400000;
    
    if (cmdheader->prdtl > 65535) {
        term_printf(term, "[AHCI] Transfer too large\n");
        return false;
    }
    
    // Настройка Command Table
    hba_cmd_table_t* cmdtbl = port->ct[slot];
    memset(cmdtbl, 0, sizeof(hba_cmd_table_t) + cmdheader->prdtl * sizeof(hba_prd_t));
    
    // Настройка PRDs
    const uint8_t* buf = (const uint8_t*)buffer;
    uint32_t remaining = count * port->sector_size;
    
    for (uint32_t i = 0; i < cmdheader->prdtl; i++) {
        uint32_t chunk = remaining > 0x400000 ? 0x400000 : remaining;
        
        cmdtbl->prdt[i].dba = (uint32_t)(uintptr_t)buf;
        cmdtbl->prdt[i].dbau = (uint32_t)((uintptr_t)buf >> 32);
        cmdtbl->prdt[i].dbc = chunk - 1;
        cmdtbl->prdt[i].i = (i == cmdheader->prdtl - 1) ? 1 : 0;
        
        buf += chunk;
        remaining -= chunk;
    }
    
    // Настройка FIS
    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)cmdtbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    
    fis->type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->device = 0x40; // LBA mode
    
    if (port->lba48 && (lba > 0x0FFFFFFF || count > 256)) {
        // LBA48
        fis->command = ATA_CMD_WRITE_DMA_EXT;
        fis->lba0 = lba & 0xFF;
        fis->lba1 = (lba >> 8) & 0xFF;
        fis->lba2 = (lba >> 16) & 0xFF;
        fis->lba3 = (lba >> 24) & 0xFF;
        fis->lba4 = (lba >> 32) & 0xFF;
        fis->lba5 = (lba >> 40) & 0xFF;
        fis->count = count & 0xFFFF;
    } else {
        // LBA28
        fis->command = ATA_CMD_WRITE_DMA;
        fis->lba0 = lba & 0xFF;
        fis->lba1 = (lba >> 8) & 0xFF;
        fis->lba2 = (lba >> 16) & 0xFF;
        fis->lba3 = 0;
        fis->lba4 = 0;
        fis->lba5 = 0;
        fis->count = count & 0xFF;
        fis->device |= ((lba >> 24) & 0x0F);
    }
    
    // Очищаем прерывания
    ahci_write_port(port, PORT_IS, 0xFFFFFFFF);
    
    // Запускаем команду
    ahci_write_port(port, PORT_CI, 1 << slot);
    
    // Ждем завершения
    bool success = ahci_wait_irq(port, slot, 5000);
    
    if (!success) {
        term_printf(term, "[AHCI] Write failed at LBA %llu\n", lba);
    }
    
    return success;
}

// Обработчик прерываний AHCI
void ahci_irq_handler(void) {
    if (!g_ahci_ctrl.initialized || g_ahci_ctrl.irq_line == 0xFF) {
        // Если инициализация не завершена или polling mode
        if (g_ahci_ctrl.irq_line != 0xFF) {
            pic_send_eoi(g_ahci_ctrl.irq_line);
        }
        return;
    }
    
    // Читаем статус прерываний HBA
    uint32_t is = mmio_read32(g_ahci_ctrl.is);
    
    if (is == 0) {
        // Spurious interrupt
        pic_send_eoi(g_ahci_ctrl.irq_line);
        return;
    }
    
    term_printf(term, "[AHCI] IRQ! HBA IS: 0x%08X\n", is);
    
    // Обрабатываем прерывания от каждого порта
    for (uint8_t i = 0; i < g_ahci_ctrl.port_count; i++) {
        if (is & (1 << i)) {
            ahci_port_t* port = &g_ahci_ctrl.ports[i];
            if (port->active && port->has_device) {
                // Читаем статус прерываний порта
                uint32_t port_is = ahci_read_port(port, PORT_IS);
                
                if (port_is) {
                    term_printf(term, "[AHCI] Port %d IS: 0x%08X\n", i, port_is);
                    
                    // Устанавливаем флаги
                    port->irq_pending = true;
                    
                    // Сохраняем завершенные слоты
                    uint32_t ci = ahci_read_port(port, PORT_CI);
                    uint32_t completed = ci ^ port->completed_slots;
                    port->completed_slots = ci;
                    
                    // Логируем завершенные команды
                    for (int slot = 0; slot < g_ahci_ctrl.cmd_slots; slot++) {
                        if (completed & (1 << slot)) {
                            term_printf(term, "[AHCI] Port %d Slot %d completed\n", i, slot);
                        }
                    }
                    
                    // Очищаем прерывания порта
                    ahci_write_port(port, PORT_IS, port_is);
                }
            }
        }
    }
    
    // Очищаем прерывания HBA
    mmio_write32(g_ahci_ctrl.is, is);
    
    // Отправляем EOI
    pic_send_eoi(g_ahci_ctrl.irq_line);
}

// Инициализация контроллера AHCI
bool ahci_init(term_t* term) {
    term_printf(term, "\n=== AHCI Driver Initialization ===\n");
    
    // Поиск контроллера AHCI через PCI
    pci_device_t* pci_dev = pci_find_class(AHCI_CLASS_MASS_STORAGE, AHCI_SUBCLASS_SATA);
    if (!pci_dev) {
        term_printf(term, "[AHCI] No AHCI controller found\n");
        return false;
    }
    
    // Проверяем, что это AHCI (Prog IF = 0x01)
    uint32_t class_rev = pci_read(pci_dev, 0x08);
    uint8_t prog_if = (class_rev >> 8) & 0xFF;
    
    if (prog_if != AHCI_PROGIF_AHCI) {
        term_printf(term, "[AHCI] Not an AHCI controller (ProgIF=0x%02X)\n", prog_if);
        free(pci_dev);
        return false;
    }
    
    term_printf(term, "[AHCI] Found controller at %02X:%02X.%X\n",
               pci_dev->bus, pci_dev->slot, pci_dev->function);
    
    // Включаем устройство
    pci_enable(pci_dev);
    pci_enable_busmaster(pci_dev);
    
    // Получаем ABAR (AHCI Base Address Register)
    uint64_t abar = pci_dev->bars[5]; // BAR5 для AHCI
    if (abar == 0 || (abar & 1)) {
        term_printf(term, "[AHCI] Invalid ABAR\n");
        free(pci_dev);
        return false;
    }
    
    abar &= ~1ULL;
    
    // Инициализируем структуру контроллера
    memset(&g_ahci_ctrl, 0, sizeof(ahci_controller_t));
    g_ahci_ctrl.mmio_base = abar;
    g_ahci_ctrl.abar = abar;
    
    // Указатели на регистры
    g_ahci_ctrl.cap = (volatile uint32_t*)(abar + 0x00);
    g_ahci_ctrl.ghc = (volatile uint32_t*)(abar + 0x04);
    g_ahci_ctrl.is = (volatile uint32_t*)(abar + 0x08);
    g_ahci_ctrl.pi = (volatile uint32_t*)(abar + 0x0C);
    
    // Получаем IRQ
    uint32_t int_line = pci_read(pci_dev, 0x3C);
    g_ahci_ctrl.irq_line = int_line & 0xFF;
    
    term_printf(term, "[AHCI] PCI IRQ Line: %d\n", g_ahci_ctrl.irq_line);
    
    // Проверяем валидность IRQ
    if (g_ahci_ctrl.irq_line == 0 || g_ahci_ctrl.irq_line > 15) {
        term_printf(term, "[AHCI] No valid IRQ, using polling mode\n");
        g_ahci_ctrl.irq_line = 0xFF;
    } else {
        // Регистрируем обработчик прерываний
        uint8_t interrupt_num = g_ahci_ctrl.irq_line + 32; // PIC offset
        
        term_printf(term, "[AHCI] Registering IRQ handler at INT %d (IRQ %d)\n",
                   interrupt_num, g_ahci_ctrl.irq_line);
        
        idt_set_gate(interrupt_num, isr_ahci, KERNEL_CODE_SEL, IDT_GATE_INT);
     
        term_printf(term, "[AHCI] IRQ %d registered", g_ahci_ctrl.irq_line);
    }
    
    // Читаем capabilities
    uint32_t cap = mmio_read32(g_ahci_ctrl.cap);
    uint32_t pi = mmio_read32(g_ahci_ctrl.pi);
    
    g_ahci_ctrl.cmd_slots = ((cap >> 8) & 0x1F) + 1;
    g_ahci_ctrl.port_count = (cap & 0x1F) + 1;
    
    term_printf(term, "[AHCI] Controller at 0x%llx\n", abar);
    term_printf(term, "[AHCI] CAP: 0x%08X, PI: 0x%08X\n", cap, pi);
    term_printf(term, "[AHCI] Command slots: %d, Ports: %d, IRQ: %d\n",
               g_ahci_ctrl.cmd_slots, g_ahci_ctrl.port_count, g_ahci_ctrl.irq_line);
    
    // Включаем AHCI mode
    uint32_t ghc = mmio_read32(g_ahci_ctrl.ghc);
    ghc |= (1 << 31); // AHCI Enable
    mmio_write32(g_ahci_ctrl.ghc, ghc);
    
    // Ждем включения
    for (int i = 0; i < 1000; i++) {
        ghc = mmio_read32(g_ahci_ctrl.ghc);
        if (ghc & (1 << 31)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    
    if (!(ghc & (1 << 31))) {
        term_printf(term, "[AHCI] Failed to enable AHCI mode\n");
        free(pci_dev);
        return false;
    }
    
    // Инициализируем порты
    for (uint8_t i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            ahci_port_t* port = &g_ahci_ctrl.ports[i];
            memset(port, 0, sizeof(ahci_port_t));
            
            port->port_num = i;
            port->clb = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_CLB);
            port->fb = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_FB);
            port->is = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_IS);
            port->ie = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_IE);
            port->cmd = (uintptr_t)(abar + 0x100 + i * 0x80 + PORT_CMD);
            port->tfd = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_TFD);
            port->sig = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_SIG);
            port->ssts = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_SSTS);
            port->sctl = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_SCTL);
            port->serr = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_SERR);
            port->sact = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_SACT);
            port->ci = (volatile uint32_t*)(abar + 0x100 + i * 0x80 + PORT_CI);
            
            // Rebase порт
            ahci_port_rebase(port);
            
            // Обнаруживаем устройство
            if (ahci_probe_port(port)) {
                ahci_start_cmd(port);
                if (ahci_identify_device(port)) {
                    term_printf(term, "[AHCI] Port %d initialized successfully\n", i);
                }
                ahci_enable_interrupts(port);
            }
        }
    }
    
    // Регистрируем обработчик прерываний
    uint8_t interrupt_num = g_ahci_ctrl.irq_line + 32;
    idt_set_gate(interrupt_num, ahci_irq_handler, KERNEL_CODE_SEL, IDT_GATE_INT);
    
    term_printf(term, "[AHCI] IRQ handler registered at INT %d\n", interrupt_num);
    
    g_ahci_ctrl.initialized = true;
    free(pci_dev);
    
    term_printf(term, "[AHCI] Initialization complete\n");
    return true;
}

// Получить контроллер
ahci_controller_t* ahci_get_controller(void) {
    return &g_ahci_ctrl;
}