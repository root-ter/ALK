#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/term/term.h"

// ==================== ОБЩИЕ КОНСТАНТЫ ====================
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_INVALID_VENDOR 0xFFFF

// ==================== СТРУКТУРЫ ====================

// Единая структура для PCI/PCIe устройств
typedef struct pci_device {
    // Идентификация
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    
    // Стандартные PCI регистры
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    
    // BAR регистры
    uint64_t bars[6];
    uint64_t bar_sizes[6];
    uint8_t bar_types[6];  // 0=32-bit, 1=64-bit, 2=I/O
    
    // Прерывания
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    
    // ========== PCIe СПЕЦИФИЧНЫЕ ПОЛЯ ==========
    bool is_pcie;
    
    // PCIe Capability
    uint8_t pcie_cap_offset;
    uint8_t pcie_type;
    uint8_t link_speed;
    uint8_t max_link_speed;
    uint16_t link_width;
    uint16_t max_link_width;
    uint8_t pcie_version;
    
    // MSI/MSI-X
    uint8_t msi_cap_offset;
    uint8_t msix_cap_offset;
    bool msi_enabled;
    bool msix_enabled;
    
    // Для связного списка
    struct pci_device* next;
} pci_device_t;

// ==================== ЕДИНЫЕ ФУНКЦИИ ====================

// Инициализация (работает и для PCI и для PCIe)
void pci_init(void);

// Чтение/запись конфигурации
uint32_t pci_read(pci_device_t* dev, uint8_t offset);
void pci_write(pci_device_t* dev, uint8_t offset, uint32_t value);

// Обнаружение устройств
int pci_scan(void);
pci_device_t* pci_find(uint16_t vendor_id, uint16_t device_id);
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);

// Управление устройствами
void pci_enable(pci_device_t* dev);
void pci_enable_busmaster(pci_device_t* dev);
void pci_enable_msi(pci_device_t* dev);

// Информация
void pci_print(term_t* term, pci_device_t* dev);
const char* pci_vendor_name(uint16_t vendor_id);
const char* pci_class_name(uint8_t class_code, uint8_t subclass);

// PCIe специфичные функции (работают только если is_pcie == true)
bool pcie_is_active(pci_device_t* dev);
const char* pcie_type_name(pci_device_t* dev);
uint8_t pcie_get_speed(pci_device_t* dev);
uint16_t pcie_get_width(pci_device_t* dev);

pci_device_t* pci_find_class_safe(uint8_t class_code, uint8_t subclass);


#endif // PCI_H