#ifndef AML_PARSER_H
#define AML_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "acpi.h"

// ==================== AML ОПКОДЫ ====================
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
#define AML_BUFFER_OP         0x11
#define AML_PACKAGE_OP        0x12
#define AML_METHOD_OP         0x14
#define AML_DEVICE_OP         0x5B82
#define AML_POWER_RESOURCE_OP 0x5B84
#define AML_PROCESSOR_OP      0x5B83
#define AML_THERMAL_ZONE_OP   0x5B85
#define AML_OPREGION_OP       0x5B80
#define AML_FIELD_OP          0x5B81
#define AML_INDEXFIELD_OP     0x5B86
#define AML_BANKFIELD_OP      0x5B87
#define AML_MUTEX_OP          0x5B01
#define AML_EVENT_OP          0x5B02
#define AML_CONDREF_OP        0x5B90
#define AML_CONNECT_OP        0x5B91
#define AML_NOTIFY_OP         0x86
#define AML_STORE_OP          0x70
#define AML_REF_OF_OP         0x71
#define AML_ADD_OP            0x72
#define AML_CONCAT_OP         0x73
#define AML_SUBTRACT_OP       0x74
#define AML_INCREMENT_OP      0x75
#define AML_DECREMENT_OP      0x76
#define AML_MULTIPLY_OP       0x77
#define AML_DIVIDE_OP         0x78
#define AML_SHIFT_LEFT_OP     0x79
#define AML_SHIFT_RIGHT_OP    0x7A
#define AML_AND_OP            0x7B
#define AML_NAND_OP           0x7C
#define AML_OR_OP             0x7D
#define AML_NOR_OP            0x7E
#define AML_XOR_OP            0x7F
#define AML_NOT_OP            0x80
#define AML_FIND_SET_LEFT_OP  0x81
#define AML_FIND_SET_RIGHT_OP 0x82
#define AML_DEREF_OF_OP       0x83
#define AML_CONCAT_RES_OP     0x84
#define AML_MOD_OP            0x85
#define AML_IF_OP             0xA0
#define AML_ELSE_OP           0xA1
#define AML_WHILE_OP          0xA2
#define AML_NOOP_OP           0xA3
#define AML_RETURN_OP         0xA4
#define AML_BREAK_OP          0xA5
#define AML_BREAK_POINT_OP    0xCC
#define AML_ONES_OP           0xFF

// ==================== ТИПЫ РЕГИОНОВ ====================
#define AML_REGION_SYSTEM_MEMORY   0x00
#define AML_REGION_SYSTEM_IO       0x01
#define AML_REGION_PCI_CONFIG      0x02
#define AML_REGION_EMBEDDED_CTRL   0x03
#define AML_REGION_SMBUS           0x04
#define AML_REGION_SYSTEM_CMOS     0x05
#define AML_REGION_PCI_BAR_TARGET  0x06
#define AML_REGION_IPMI            0x07
#define AML_REGION_GENERAL_PURPOSE 0x08
#define AML_REGION_GENERIC_SERIAL  0x09
#define AML_REGION_PCC             0x0A

// ==================== ТИПЫ ПОЛЕЙ ====================
#define AML_FIELD_NOLOCK       0x00
#define AML_FIELD_LOCK         0x01
#define AML_FIELD_PRESERVE     0x00
#define AML_FIELD_WRITE_AS_ONES 0x02
#define AML_FIELD_WRITE_AS_ZEROS 0x03

// ==================== СТРУКТУРЫ ====================

// Информация о GPE (General Purpose Event)
typedef struct {
    uint8_t gpe_number;      // Номер GPE (из _Lxx или _Exx)
    uint16_t gpe_bit;        // Бит в регистре GPE (обычно == gpe_number)
    uint8_t gpe_type;        // 0 = _Lxx (level), 1 = _Exx (edge)
    uint32_t gpe_blk_addr;   // Адрес блока GPE из FADT
    bool found;
    bool is_power_button;
} aml_gpe_info_t;

// Информация о кнопке питания (расширенная)
typedef struct {
    // EC Space
    uint8_t ec_space_id;
    uint8_t power_button_bit;
    uint16_t power_button_offset;
    uint8_t power_button_region;
    
    // GPE
    aml_gpe_info_t gpe;
    
    // PWRB Device
    bool has_pwrb_device;
    char pwrb_path[128];
    
    // PM1 регистры (fallback)
    uint16_t pm1_evt_blk;
    uint8_t pm1_pwr_btn_bit;
    
    bool found;
    bool is_legacy;
} power_button_info_t;

// Контекст AML парсера
typedef struct {
    // DSDT
    uint8_t* dsdt_data;
    size_t dsdt_len;
    
    // SSDT
    uint8_t* ssdt_data[16];
    size_t ssdt_len[16];
    int ssdt_count;
    
    // Результаты поиска
    power_button_info_t pwr_info;
    aml_gpe_info_t gpe_info[32];
    int gpe_count;
    
    // Текущий контекст обхода
    char current_scope[256];
    int scope_depth;
    
    // Флаги
    bool initialized;
} aml_context_t;

// ==================== ПРОТОТИПЫ ====================

// Инициализация и поиск
bool aml_init_context(aml_context_t *ctx, acpi_context_t *acpi_ctx);
bool aml_find_power_button(aml_context_t *ctx);
bool aml_get_power_button_info(power_button_info_t *info);
bool aml_get_gpe_info(aml_gpe_info_t *info, int index);

// Обход AML
void aml_walk_tables(aml_context_t *ctx);
void aml_walk_scope(aml_context_t *ctx, uint8_t *data, size_t len, int depth);

// Парсинг конкретных узлов
void aml_parse_device(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos);
void aml_parse_scope(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos);
void aml_parse_method(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos);
void aml_parse_opregion(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos);
void aml_parse_field(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos);

// Извлечение имени
int aml_extract_namestring(uint8_t *data, size_t len, size_t *pos, char *buf, size_t buf_len);
bool aml_name_equals(const char *name1, const char *name2);
void aml_resolve_path(char *dest, const char *scope, const char *name, size_t max_len);

// Отладка
void aml_hex_dump(uint8_t *data, size_t len, const char *label);
void aml_print_info(aml_context_t *ctx);

#endif // AML_PARSER_H
