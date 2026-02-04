#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ==================== ACPI SDT HEADER ====================
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

// ==================== RSDP ====================
typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;          // Для ACPI 1.0
    uint32_t length;                // Для ACPI 2.0+
    uint64_t xsdt_address;          // Для ACPI 2.0+
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} rsdp_t;

// ==================== RSDT/XSDT ====================
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    // Для RSDT: массив uint32_t указателей
    // Для XSDT: массив uint64_t указателей
} rsdt_t;

// ==================== FADT ====================
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    
    uint8_t  reserved;
    
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_len;
    uint8_t  gpe1_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    
    uint8_t  day_alrm;
    uint8_t  month_alrm;
    uint8_t  century;
    
    // ACPI 2.0+ fields
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    
    // ACPI 3.0+ fields
    uint8_t  reset_reg[12];
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
    
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    
    uint8_t  x_pm1a_evt_blk[12];
    uint8_t  x_pm1b_evt_blk[12];
    uint8_t  x_pm1a_cnt_blk[12];
    uint8_t  x_pm1b_cnt_blk[12];
    uint8_t  x_pm2_cnt_blk[12];
    uint8_t  x_pm_tmr_blk[12];
    uint8_t  x_gpe0_blk[12];
    uint8_t  x_gpe1_blk[12];
    
    // ACPI 5.0+ fields
    uint8_t  sleep_control_reg[12];
    uint8_t  sleep_status_reg[12];
} fadt_t;

// ==================== MADT ====================
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
} madt_t;

// Типы записей MADT
#define MADT_TYPE_LOCAL_APIC       0
#define MADT_TYPE_IO_APIC          1
#define MADT_TYPE_INTERRUPT_SOURCE 2
#define MADT_TYPE_NMI_SOURCE       3
#define MADT_TYPE_LOCAL_APIC_NMI   4
#define MADT_TYPE_LOCAL_APIC_ADDR  5
#define MADT_TYPE_IOSAPIC          6
#define MADT_TYPE_LOCAL_SAPIC      7
#define MADT_TYPE_PLATFORM_INT     8
#define MADT_TYPE_LOCAL_X2APIC     9
#define MADT_TYPE_LOCAL_X2APIC_NMI 10

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} madt_header_t;

typedef struct __attribute__((packed)) {
    madt_header_t header;
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags; // bit 0 = Processor Enabled
} madt_local_apic_t;

typedef struct __attribute__((packed)) {
    madt_header_t header;
    uint8_t  io_apic_id;
    uint8_t  reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} madt_io_apic_t;

typedef struct __attribute__((packed)) {
    madt_header_t header;
    uint8_t  bus;
    uint8_t  source;
    uint32_t interrupt;
    uint16_t flags;
} madt_interrupt_source_t;

// ==================== MCFG ====================
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t reserved;
} mcfg_t;

typedef struct __attribute__((packed)) {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} mcfg_entry_t;

// ==================== SSDT/DSDT ====================
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    // AML байткод следует сразу после заголовка
} ssdt_t;

// ==================== ГЛОБАЛЬНЫЕ СТРУКТУРЫ ====================
typedef struct {
    rsdp_t* rsdp;
    rsdt_t* rsdt;
    fadt_t* fadt;
    madt_t* madt;
    mcfg_t* mcfg;
    ssdt_t* dsdt;
    ssdt_t* ssdts[16];
    int ssdt_count;
    
    // Информация из MADT
    uint32_t local_apic_addr;
    uint32_t io_apic_addr;
    uint32_t gsi_base;
    
    // Флаги
    bool xsdt_present;
    bool acpi_enabled;
} acpi_context_t;

// ==================== ФУНКЦИИ ====================

// Инициализация ACPI
bool acpi_init(void);

// Получить контекст ACPI
acpi_context_t* acpi_get_context(void);

// Найти таблицу по сигнатуре
void* acpi_find_table(const char* signature);

// Включить ACPI
void acpi_enable(void);

// Выключить ACPI
void acpi_disable(void);

// Перезагрузить систему через ACPI
void acpi_reboot(void);

// Выключить систему через ACPI
void acpi_shutdown(void);

// Получить количество CPU из MADT
int acpi_get_cpu_count(void);

// Получить адрес IOAPIC
uint32_t acpi_get_ioapic_address(void);

// Получить адрес Local APIC
uint32_t acpi_get_local_apic_address(void);

// Получить таблицу MCFG для PCIe
mcfg_t* acpi_get_mcfg(void);

// Дамп информации ACPI
void acpi_dump_info(void);

// Проверка контрольной суммы
bool acpi_checksum_valid(const uint8_t* data, size_t length);

#endif // ACPI_H 