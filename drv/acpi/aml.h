#ifndef AML_PARSER_H
#define AML_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "acpi.h"

// Базовые AML типы (дополним)
#define AML_ZERO_OP           0x00
#define AML_ONE_OP            0x01
#define AML_ALIAS_OP          0x06
#define AML_NAME_OP           0x08
#define AML_BYTE_PREFIX       0x0A
#define AML_WORD_PREFIX       0x0B
#define AML_DWORD_PREFIX      0x0C
#define AML_STRING_PREFIX     0x0D
#define AML_QWORD_PREFIX      0x0E
#define AML_SCOPE_OP          0x10
#define AML_DEVICE_OP         0x5B
#define AML_METHOD_OP         0x14
#define AML_PACKAGE_OP        0x12
#define AML_BUFFER_OP         0x11
#define AML_OPREGION_OP       0x80
#define AML_FIELD_OP          0x81
#define AML_INDEXFIELD_OP     0x86
#define AML_BANKFIELD_OP      0x87
#define AML_POWER_RESOURCE_OP 0x84

// Опкоды EC
#define AML_EC_OP             0x82  // EmbeddedControl
#define AML_REGION_OP         0x80  // OperationRegion

// Имена устройств для поиска
#define EC_DEVICE_NAMES       "EC", "H_EC", "LID0", "EC0", "EC1"
#define POWER_BUTTON_NAMES    "_PWR", "PWRB", "PWRN", "PBTN", "PWRF"

// Для поиска кнопки питания
typedef struct {
    uint8_t ec_space_id;      // EC адресное пространство
    uint8_t power_button_bit; // Бит кнопки питания
    uint16_t power_button_offset; // Смещение в EC Space
    uint8_t power_button_region;  // Регион (0=IO, 1=Memory, 2=PCI)
    bool found;
    bool is_legacy;          // Использует ли legacy методы
} power_button_info_t;

// Контекст парсера
typedef struct {
    uint8_t* dsdt_data;
    size_t dsdt_len;
    uint8_t* ssdt_data[16];
    size_t ssdt_len[16];
    int ssdt_count;
    power_button_info_t pwr_info;
} aml_context_t;

// Прототипы функций
bool aml_find_power_button(uint8_t* aml_data, size_t aml_len, 
                          power_button_info_t* info);
bool aml_parse_dsdt(aml_context_t* ctx);
void aml_hex_dump(uint8_t* data, size_t len, const char* label);
bool aml_init_context(aml_context_t* ctx, acpi_context_t* acpi_ctx);
bool aml_get_power_button_info(power_button_info_t* info);

#endif