#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../base/term/term.h"
#include "../pci/pci.h"

// ==================== AHCI КОНСТАНТЫ ====================
#define AHCI_CLASS_MASS_STORAGE 0x01
#define AHCI_SUBCLASS_SATA      0x06
#define AHCI_PROGIF_AHCI        0x01

// Регистры HBA (Host Bus Adapter)
#define HBA_CAP     0x00    // Capabilities
#define HBA_GHC     0x04    // Global HBA Control
#define HBA_IS      0x08    // Interrupt Status
#define HBA_PI      0x0C    // Ports Implemented
#define HBA_VS      0x10    // Version
#define HBA_CCC_CTL 0x14    // Command Completion Coalescing Control
#define HBA_CCC_PTS 0x18    // CCC Ports
#define HBA_EM_LOC  0x1C    // Enclosure Management Location
#define HBA_EM_CTL  0x20    // Enclosure Management Control
#define HBA_CAP2    0x24    // Extended Capabilities
#define HBA_BOHC    0x28    // BIOS/OS Handoff Control

// Регистры порта
#define PORT_CLB    0x00    // Command List Base Address
#define PORT_CLBU   0x04    // Command List Base Address Upper
#define PORT_FB     0x08    // FIS Base Address
#define PORT_FBU    0x0C    // FIS Base Address Upper
#define PORT_IS     0x10    // Interrupt Status
#define PORT_IE     0x14    // Interrupt Enable
#define PORT_CMD    0x18    // Command and Status
#define PORT_TFD    0x20    // Task File Data
#define PORT_SIG    0x24    // Signature
#define PORT_SSTS   0x28    // Serial ATA Status
#define PORT_SCTL   0x2C    // Serial ATA Control
#define PORT_SERR   0x30    // Serial ATA Error
#define PORT_SACT   0x34    // Serial ATA Active
#define PORT_CI     0x38    // Command Issue
#define PORT_SNTF   0x3C    // Serial ATA Notification

// Бит CMD регистра
#define CMD_ST      0x0001  // Start (исполнение списка команд)
#define CMD_FRE     0x0010  // FIS Receive Enable
#define CMD_FR      0x4000  // FIS Receive Running
#define CMD_CR      0x8000  // Command List Running

// Бит CAP регистра
#define CAP_S64A    0x80000000  // 64-bit Addressing Support
#define CAP_SNCQ    0x40000000  // Native Command Queuing
#define CAP_SSNTF   0x20000000  // SNotification Register
#define CAP_SMPS    0x10000000  // Mechanical Presence Switch
#define CAP_SSS     0x08000000  // Staggered Spin-up
#define CAP_SALP    0x04000000  // Aggressive Link Power Management
#define CAP_SAL     0x02000000  // Activity LED
#define CAP_SCLO    0x01000000  // Command List Override
#define CAP_ISS     (0xf << 20) // Interface Speed Support
#define CAP_SAM     0x00200000  // AHCI Mode Only
#define CAP_SPM     0x00100000  // Port Multiplier
#define CAP_FBSS    0x00080000  // FIS-based Switching
#define CAP_PMD     0x00040000  // PIO Multiple DRQ Block
#define CAP_SSC     0x00020000  // Slumber State Capable
#define CAP_PSSC    0x00010000  // Partial State Capable
#define CAP_NCS     (0x1f << 8) // Number of Command Slots
#define CAP_CCCS    0x00000080  // Command Completion Coalescing
#define CAP_EMS     0x00000040  // Enclosure Management
#define CAP_SXS     0x00000020  // External SATA
#define CAP_NP      (0x1f << 0) // Number of Ports

// Типы FIS
#define FIS_TYPE_REG_H2D    0x27    // Register FIS - Host to Device
#define FIS_TYPE_REG_D2H    0x34    // Register FIS - Device to Host
#define FIS_TYPE_DMA_ACT    0x39    // DMA Activate FIS - Device to Host
#define FIS_TYPE_DMA_SETUP  0x41    // DMA Setup FIS - Bidirectional
#define FIS_TYPE_DATA       0x46    // Data FIS - Bidirectional
#define FIS_TYPE_BIST       0x58    // BIST Activate FIS - Bidirectional
#define FIS_TYPE_PIO_SETUP  0x5F    // PIO Setup FIS - Device to Host
#define FIS_TYPE_DEV_BITS   0xA1    // Set Device Bits FIS - Device to Host

// Команды ATA
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_DMA    0xC8
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA   0xCA
#define ATA_CMD_WRITE_DMA_EXT 0x35

// Структура FIS (Frame Information Structure)
typedef struct __attribute__((packed)) {
    uint8_t type;           // Тип FIS
    uint8_t pmport:4;       // Port Multiplier
    uint8_t rsv0:3;
    uint8_t c:1;            // Update Command Register
    uint8_t command;        // Command Register
    uint8_t feature_low;    // Feature Register (7:0)
    
    uint8_t lba0;           // LBA Low (7:0)
    uint8_t lba1;           // LBA Mid (15:8)
    uint8_t lba2;           // LBA High (23:16)
    uint8_t device;         // Device Register
    
    uint8_t lba3;           // LBA Low (31:24)
    uint8_t lba4;           // LBA Mid (39:32)
    uint8_t lba5;           // LBA High (47:40)
    uint8_t feature_high;   // Feature Register (15:8)
    
    uint16_t count;         // Count Register
    uint8_t icc;            // Isochronous Command Completion
    uint8_t control;        // Control Register
    
    uint32_t rsv1;          // Reserved
} fis_reg_h2d_t;

typedef struct __attribute__((packed)) {
    uint8_t type;           // FIS_TYPE_REG_D2H
    uint8_t pmport:4;
    uint8_t rsv0:2;
    uint8_t i:1;            // Interrupt bit
    uint8_t rsv1:1;
    uint8_t status;         // Status Register
    uint8_t error;          // Error Register
    
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t rsv2;
    
    uint16_t count;
    uint16_t rsv3;
    
    uint32_t rsv4;
} fis_reg_d2h_t;

typedef struct __attribute__((packed)) {
    uint8_t type;           // FIS_TYPE_DATA
    uint8_t pmport:4;
    uint8_t rsv0:4;
    uint16_t rsv1;
    uint32_t data[0];       // Переменная длина (до 2048 DWORDs)
} fis_data_t;

// Структура Command Header
typedef struct __attribute__((packed)) {
    uint8_t cfl:5;          // Command FIS Length (в DWORDs, 2-16)
    uint8_t a:1;            // ATAPI
    uint8_t w:1;            // Write
    uint8_t p:1;            // Prefetchable
    
    uint8_t r:1;            // Reset
    uint8_t b:1;            // BIST
    uint8_t c:1;            // Clear Busy upon R_OK
    uint8_t rsv0:1;
    uint8_t pmp:4;          // Port Multiplier Port
    
    uint16_t prdtl;         // Physical Region Descriptor Table Length
    volatile uint32_t prdbc; // PRD Byte Count
    
    uint32_t ctba;          // Command Table Descriptor Base Address
    uint32_t ctbau;         // Command Table Descriptor Base Address Upper
    
    uint32_t rsv1[4];
} hba_cmd_header_t;

// Структура PRD (Physical Region Descriptor)
typedef struct __attribute__((packed)) {
    uint32_t dba;           // Data Base Address
    uint32_t dbau;          // Data Base Address Upper
    uint32_t rsv0;
    uint32_t dbc:22;        // Byte Count (0 = 4MiB)
    uint32_t rsv1:9;
    uint32_t i:1;           // Interrupt on Completion
} hba_prd_t;

// Структура Command Table
typedef struct __attribute__((packed)) {
    uint8_t cfis[64];       // Command FIS
    uint8_t acmd[16];       // ATAPI Command (12 or 16 bytes)
    uint8_t rsv[48];        // Reserved
    hba_prd_t prdt[1];      // Physical Region Descriptor Table
} hba_cmd_table_t;

// Структура порта AHCI
typedef struct {
    volatile uint32_t* clb;     // Command List Base
    volatile uint32_t* fb;      // FIS Base
    volatile uint32_t* is;      // Interrupt Status
    volatile uint32_t* ie;      // Interrupt Enable
    uintptr_t cmd;     // Command
    volatile uint32_t* tfd;     // Task File Data
    volatile uint32_t* sig;     // Signature
    volatile uint32_t* ssts;    // SATA Status
    volatile uint32_t* sctl;    // SATA Control
    volatile uint32_t* serr;    // SATA Error
    volatile uint32_t* sact;    // SATA Active
    volatile uint32_t* ci;      // Command Issue
    
    // Указатели на выделенные структуры
    hba_cmd_header_t* cl;
    void* fis;
    hba_cmd_table_t* ct[32];    // Command tables для каждого слота
    
    // Состояние порта
    uint8_t port_num;
    bool active;
    bool has_device;
    bool lba48;
    uint8_t sata_speed;
    uint64_t sectors;
    uint32_t sector_size;
    
    // Для обработки прерываний
    volatile bool irq_pending;
    volatile uint32_t completed_slots;
} ahci_port_t;

// Структура контроллера AHCI
typedef struct {
    uintptr_t mmio_base;
    uintptr_t abar;             // AHCI Base Address Register
    
    // Регистры HBA
    volatile uint32_t* cap;
    volatile uint32_t* ghc;
    volatile uint32_t* is;
    volatile uint32_t* pi;
    
    // Порты
    ahci_port_t ports[32];
    uint8_t port_count;
    uint8_t cmd_slots;
    
    // IRQ
    uint8_t irq_line;
    
    // Состояние
    bool initialized;
} ahci_controller_t;

// ==================== ФУНКЦИИ ====================

// Инициализация
bool ahci_init(term_t* term);
ahci_controller_t* ahci_get_controller(void);

// Обнаружение устройств
bool ahci_probe_port(ahci_port_t* port);
bool ahci_identify_device(ahci_port_t* port);

// Прерывания
void ahci_irq_handler(void);
void ahci_enable_interrupts(ahci_port_t* port);
void ahci_disable_interrupts(ahci_port_t* port);
bool ahci_wait_irq(ahci_port_t* port, uint32_t slot, uint32_t timeout_ms);

// Операции чтения/записи
bool ahci_read(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);
bool ahci_write(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);

// Утилиты
void ahci_port_rebase(ahci_port_t* port);
void ahci_start_cmd(ahci_port_t* port);
void ahci_stop_cmd(ahci_port_t* port);
uint32_t ahci_read_port(ahci_port_t* port, uint32_t offset);
void ahci_write_port(ahci_port_t* port, uint32_t offset, uint32_t value);
uint32_t ahci_read_reg(uintptr_t base, uint32_t offset);
void ahci_write_reg(uintptr_t base, uint32_t offset, uint32_t value);

#endif // AHCI_H