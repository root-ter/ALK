#include "pci.h"
#include "../io/io.h"
#include "../../base/mem/mem.h"
#include "../../base/term/tio.h"
#include "../../base/term/term.h"
#include "../../libc/string.h"

static void pci_scan_bus(uint8_t bus);
uint32_t pci_read_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

// ==================== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ====================
static pci_device_t* pci_devices = NULL;
static int device_count = 0;

// ==================== БАЗОВЫЕ ОПЕРАЦИИ ====================

uint32_t pci_read_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    // Формируем адрес
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    
    // Отправляем адрес
    outl(0xCF8, address);
    
    // Читаем данные
    return inl(0xCFC);
}

// Формирование адреса
static uint32_t pci_make_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1u << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC);
}

// Чтение конфигурации
uint32_t pci_read(pci_device_t* dev, uint8_t offset) {
    uint32_t addr = pci_make_addr(dev->bus, dev->slot, dev->function, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

// Запись конфигурации
void pci_write(pci_device_t* dev, uint8_t offset, uint32_t value) {
    uint32_t addr = pci_make_addr(dev->bus, dev->slot, dev->function, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, value);
}

// Быстрое чтение полей
static uint16_t pci_read_vendor(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t addr = pci_make_addr(bus, slot, func, 0x00);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA) & 0xFFFF;
}

// ==================== ОБНАРУЖЕНИЕ PCI/PCIe ====================

// Поиск Capability
static uint8_t pci_find_cap(pci_device_t* dev, uint8_t cap_id) {
    // Читаем указатель на capabilities
    uint8_t cap_ptr = (pci_read(dev, 0x34) >> 8) & 0xFF;
    if (!cap_ptr) return 0;
    
    uint8_t offset = cap_ptr;
    while (offset >= 0x40) {
        uint8_t id = pci_read(dev, offset) & 0xFF;
        if (id == cap_id) return offset;
        offset = (pci_read(dev, offset + 1) >> 8) & 0xFF;
    }
    
    return 0;
}

// Обнаружение PCIe
static void pci_detect_pcie(pci_device_t* dev) {
    dev->pcie_cap_offset = pci_find_cap(dev, 0x10); // PCIe Capability ID
    
    if (!dev->pcie_cap_offset) {
        dev->is_pcie = false;
        return;
    }
    
    dev->is_pcie = true;
    
    // Читаем PCIe Capability
    uint32_t pcie_cap = pci_read(dev, dev->pcie_cap_offset);
    dev->pcie_version = (pcie_cap >> 16) & 0xF;
    dev->pcie_type = (pcie_cap >> 20) & 0xF;
    
    // Link Capabilities
    uint32_t link_cap = pci_read(dev, dev->pcie_cap_offset + 0x0C);
    dev->max_link_speed = link_cap & 0xF;
    dev->max_link_width = (link_cap >> 4) & 0x3F;
    
    // Link Status
    uint16_t link_status = pci_read(dev, dev->pcie_cap_offset + 0x12) & 0xFFFF;
    dev->link_speed = link_status & 0xF;
    dev->link_width = (link_status >> 4) & 0x3F;
    
    // MSI/MSI-X
    dev->msi_cap_offset = pci_find_cap(dev, 0x05); // MSI
    dev->msix_cap_offset = pci_find_cap(dev, 0x11); // MSI-X
}

// Обнаружение BAR регистров
static void pci_detect_bars(pci_device_t* dev) {
    // Сохраняем команду
    uint16_t saved_cmd = dev->command;
    pci_write(dev, 0x04, saved_cmd & ~0x07); // Отключаем I/O и Memory
    
    for (int i = 0; i < 6; i++) {
        uint8_t offset = 0x10 + i * 4;
        uint32_t bar = pci_read(dev, offset);
        
        if (bar == 0) {
            dev->bars[i] = 0;
            dev->bar_sizes[i] = 0;
            dev->bar_types[i] = 0;
            continue;
        }
        
        // Определяем тип BAR
        if (bar & 0x01) {
            // I/O Space BAR
            dev->bar_types[i] = 2;
            pci_write(dev, offset, 0xFFFFFFFF);
            uint32_t size = pci_read(dev, offset);
            size = ~(size & 0xFFFFFFFC) + 1;
            pci_write(dev, offset, bar);
            dev->bars[i] = bar & 0xFFFFFFFC;
            dev->bar_sizes[i] = size;
        } else {
            // Memory Space BAR
            uint8_t type = (bar >> 1) & 0x03;
            
            if (type == 0x00) { // 32-bit
                dev->bar_types[i] = 0;
                pci_write(dev, offset, 0xFFFFFFFF);
                uint32_t size = pci_read(dev, offset);
                size = ~(size & 0xFFFFFFF0) + 1;
                pci_write(dev, offset, bar);
                dev->bars[i] = bar & 0xFFFFFFF0;
                dev->bar_sizes[i] = size;
            } else if (type == 0x02) { // 64-bit
                dev->bar_types[i] = 1;
                if (i < 5) {
                    // Обрабатываем как 64-bit
                    uint32_t bar_high = pci_read(dev, offset + 4);
                    uint64_t full_bar = ((uint64_t)bar_high << 32) | (bar & 0xFFFFFFF0);
                    
                    // Записываем все единицы
                    pci_write(dev, offset, 0xFFFFFFFF);
                    pci_write(dev, offset + 4, 0xFFFFFFFF);
                    
                    uint32_t size_low = pci_read(dev, offset);
                    uint32_t size_high = pci_read(dev, offset + 4);
                    uint64_t full_size = ((uint64_t)size_high << 32) | (size_low & 0xFFFFFFF0);
                    full_size = ~full_size + 1;
                    
                    // Восстанавливаем
                    pci_write(dev, offset, bar);
                    pci_write(dev, offset + 4, bar_high);
                    
                    dev->bars[i] = full_bar;
                    dev->bar_sizes[i] = full_size;
                    i++; // Пропускаем следующий BAR
                }
            }
        }
    }
    
    // Восстанавливаем команду
    pci_write(dev, 0x04, saved_cmd);
}

// Обнаружение одного устройства
static pci_device_t* pci_probe(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read_vendor(bus, slot, func);
    if (vendor == PCI_INVALID_VENDOR) return NULL;
    
    pci_device_t* dev = malloc(sizeof(pci_device_t));
    memset(dev, 0, sizeof(pci_device_t));
    
    dev->bus = bus;
    dev->slot = slot;
    dev->function = func;
    dev->vendor_id = vendor;
    
    // Читаем основные регистры
    uint32_t reg00 = pci_read(dev, 0x00);
    dev->device_id = (reg00 >> 16) & 0xFFFF;
    
    uint32_t reg04 = pci_read(dev, 0x04);
    dev->command = reg04 & 0xFFFF;
    dev->status = (reg04 >> 16) & 0xFFFF;
    
    uint32_t reg08 = pci_read(dev, 0x08);
    dev->revision_id = reg08 & 0xFF;
    dev->prog_if = (reg08 >> 8) & 0xFF;
    dev->subclass = (reg08 >> 16) & 0xFF;
    dev->class_code = (reg08 >> 24) & 0xFF;
    
    uint32_t reg0C = pci_read(dev, 0x0C);
    dev->cache_line = reg0C & 0xFF;
    dev->latency_timer = (reg0C >> 8) & 0xFF;
    dev->header_type = (reg0C >> 16) & 0xFF;
    dev->bist = (reg0C >> 24) & 0xFF;
    
    uint32_t reg3C = pci_read(dev, 0x3C);
    dev->interrupt_line = reg3C & 0xFF;
    dev->interrupt_pin = (reg3C >> 8) & 0xFF;
    
    // Обнаруживаем BAR
    pci_detect_bars(dev);
    
    // Обнаруживаем PCIe
    pci_detect_pcie(dev);
    
    // Добавляем в список
    dev->next = pci_devices;
    pci_devices = dev;
    device_count++;
    
    return dev;
}

// Сканирование функции
static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    pci_probe(bus, slot, func);
    
    // Если это PCI-to-PCI мост, сканируем вторичную шину
    pci_device_t* dev = pci_devices; // Только что добавленное устройство
    if (dev->class_code == 0x06 && dev->subclass == 0x04) {
        uint8_t secondary_bus = (pci_read(dev, 0x18) >> 8) & 0xFF;
        if (secondary_bus != 0) {
            pci_scan_bus(secondary_bus);
        }
    }
}

// Сканирование устройства
static void pci_scan_device(uint8_t bus, uint8_t slot) {
    // Проверяем функцию 0
    if (pci_read_vendor(bus, slot, 0) == PCI_INVALID_VENDOR) return;
    
    pci_scan_function(bus, slot, 0);
    
    // Проверяем multifunction
    pci_device_t* dev = pci_devices; // Только что добавленное
    if (dev->header_type & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            if (pci_read_vendor(bus, slot, func) != PCI_INVALID_VENDOR) {
                pci_scan_function(bus, slot, func);
            }
        }
    }
}

// Сканирование шины
static void pci_scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        pci_scan_device(bus, slot);
    }
}

// ==================== ПУБЛИЧНЫЕ ФУНКЦИИ ====================

// Инициализация PCI/PCIe
void pci_init(void) {
    // Проверяем multifunction у host bridge (0:0:0)
    pci_device_t temp_dev = {.bus = 0, .slot = 0, .function = 0};
    uint8_t header_type = (pci_read(&temp_dev, 0x0C) >> 16) & 0xFF;
    
    if (header_type & 0x80) {
        // Multiple host controllers
        for (uint8_t func = 0; func < 8; func++) {
            if (pci_read_vendor(0, 0, func) != PCI_INVALID_VENDOR) {
                pci_scan_bus(func);
            }
        }
    } else {
        // Single host controller
        pci_scan_bus(0);
    }
}

// Сканирование (дополнительное, если нужно пересканировать)
int pci_scan(void) {
    // Очищаем старый список
    pci_device_t* dev = pci_devices;
    while (dev) {
        pci_device_t* next = dev->next;
        free(dev);
        dev = next;
    }
    pci_devices = NULL;
    device_count = 0;
    
    // Сканируем заново
    pci_init();
    return device_count;
}

// Поиск устройства
pci_device_t* pci_find(uint16_t vendor_id, uint16_t device_id) {
    pci_device_t* dev = pci_devices;
    while (dev) {
        if (dev->vendor_id == vendor_id && dev->device_id == device_id) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

// Поиск по классу
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass) {
    pci_device_t* dev = pci_devices;
    while (dev) {
        if (dev->class_code == class_code && dev->subclass == subclass) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

// Включение устройства
void pci_enable(pci_device_t* dev) {
    dev->command |= (1 << 0) | (1 << 1) | (1 << 2); // I/O, Memory, Bus Master
    pci_write(dev, 0x04, dev->command);
}

// Включение Bus Mastering
void pci_enable_busmaster(pci_device_t* dev) {
    dev->command |= (1 << 2);
    pci_write(dev, 0x04, dev->command);
}

// Включение MSI (если поддерживается)
void pci_enable_msi(pci_device_t* dev) {
    if (!dev->is_pcie || !dev->msi_cap_offset) return;
    
    // Настраиваем MSI
    uint32_t msi_control = pci_read(dev, dev->msi_cap_offset);
    msi_control |= 0x00010001; // Enable + 64-bit
    pci_write(dev, dev->msi_cap_offset, msi_control);
    
    // Адрес MSI (фиксированный APIC)
    pci_write(dev, dev->msi_cap_offset + 0x04, 0xFEE00000);
    pci_write(dev, dev->msi_cap_offset + 0x08, 0x00000000);
    
    // Вектор прерывания (пока 32)
    pci_write(dev, dev->msi_cap_offset + 0x0C, 32);
    
    dev->msi_enabled = true;
}

// ==================== PCIe СПЕЦИФИЧНЫЕ ФУНКЦИИ ====================

// Проверка активности PCIe
bool pcie_is_active(pci_device_t* dev) {
    return dev->is_pcie;
}

// Получение типа PCIe
const char* pcie_type_name(pci_device_t* dev) {
    if (!dev->is_pcie) return "PCI";
    
    switch (dev->pcie_type) {
        case 0x0: return "Endpoint";
        case 0x1: return "Legacy Endpoint";
        case 0x4: return "Root Port";
        case 0x5: return "Upstream Port";
        case 0x6: return "Downstream Port";
        case 0x7: return "PCIe-to-PCI Bridge";
        case 0x8: return "PCI-to-PCIe Bridge";
        default: return "Unknown";
    }
}

// Получение скорости
uint8_t pcie_get_speed(pci_device_t* dev) {
    return dev->is_pcie ? dev->link_speed : 0;
}

// Получение ширины
uint16_t pcie_get_width(pci_device_t* dev) {
    return dev->is_pcie ? dev->link_width : 0;
}

// ==================== ИНФОРМАЦИОННЫЕ ФУНКЦИИ ====================

// Имя вендора
const char* pci_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x10EC: return "Realtek";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "Red Hat";
        default: return "Unknown";
    }
}

// Имя класса
const char* pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x01:
            switch (subclass) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x06: return "SATA Controller";
                default: return "Storage Controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "Ethernet Controller";
                default: return "Network Controller";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA Controller";
                case 0x02: return "3D Controller";
                default: return "Display Controller";
            }
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                default: return "Bridge";
            }
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB Controller";
                default: return "Serial Controller";
            }
        default: return "Unknown Device";
    }
}

// Вывод информации об устройстве
void pci_print(term_t* term, pci_device_t* dev) {
    if (!dev) return;

    (void)term;
    
    tio_printf("PCI %02X:%02X.%X: %04X:%04X [%02X:%02X] %s - %s\n",
           dev->bus, dev->slot, dev->function,
           dev->vendor_id, dev->device_id,
           dev->class_code, dev->subclass,
           pci_vendor_name(dev->vendor_id),
           pci_class_name(dev->class_code, dev->subclass));
    
    if (dev->is_pcie) {
        tio_printf("  PCIe %s, Gen%d x%d\n",
               pcie_type_name(dev),
               dev->link_speed,
               dev->link_width);
    }
}

// Безопасный поиск PCI устройства
pci_device_t* pci_find_class_safe(uint8_t class_code, uint8_t subclass) {
    // Ограничиваем поиск только существующими шинами
    for (uint8_t bus = 0; bus < 3; bus++) {  // bus 0-2 обычно достаточно
        for (uint8_t slot = 0; slot < 8; slot++) {  // slot 0-7
            for (uint8_t func = 0; func < 1; func++) {  // только function 0
                
                // Проверяем существует ли устройство
                uint32_t vendor = pci_read_addr(bus, slot, func, 0x00);
                if (vendor == 0xFFFFFFFF || vendor == 0x00000000) {
                    continue;  // Устройства нет
                }
                
                // Читаем класс/подкласс
                uint32_t class_rev = pci_read_addr(bus, slot, func, 0x08);
                uint8_t found_class = (class_rev >> 24) & 0xFF;
                uint8_t found_subclass = (class_rev >> 16) & 0xFF;
                
                if (found_class == class_code && found_subclass == subclass) {
                    pci_device_t* dev = (pci_device_t*)malloc(sizeof(pci_device_t));
                    if (!dev) return NULL;
                    
                    memset(dev, 0, sizeof(pci_device_t));
                    dev->bus = bus;
                    dev->slot = slot;
                    dev->function = func;
                    dev->vendor_id = vendor & 0xFFFF;
                    dev->device_id = (vendor >> 16) & 0xFFFF;
                    
                    // Заполняем BARs
                    for (int i = 0; i < 6; i++) {
                        dev->bars[i] = pci_read(dev, 0x10 + i * 4);
                    }
                    
                    return dev;
                }
            }
        }
    }
    return NULL;
}