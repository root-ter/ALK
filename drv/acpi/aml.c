#include "aml.h"
#include "../../libc/string.h"
#include "../../base/term/term.h"

extern term_t* term;

// Глобальный контекст
static aml_context_t g_aml_ctx;

// Вспомогательная: пропуск пустых имен
static size_t skip_lead_char(uint8_t* data, size_t offset) {
    if (data[offset] == 0x5C) return 1; // Root Char
    if (data[offset] == 0x5E) return 1; // Parent Prefix
    return 0;
}

// Парсинг имени (4 или 6 байт)
static size_t parse_name(uint8_t* data, size_t offset, char* name_buf, size_t max_len) {
    size_t start = offset;
    
    // Пропускаем Root/Parent префиксы
    offset += skip_lead_char(data, offset);
    
    // Имя может быть 4 байта (заканчивается на 0x00) или 6 байт (двойное)
    if (offset + 4 <= start + 8 && offset < max_len) {
        size_t name_len = 0;
        for (int i = 0; i < 4 && (offset + i) < max_len; i++) {
            if (data[offset + i] == 0x00) break;
            name_buf[i] = data[offset + i];
            name_len++;
        }
        name_buf[name_len] = '\0';
        return offset + 4;
    }
    
    return offset;
}

// Поиск Embedded Controller в AML
static bool find_ec_in_aml(uint8_t* data, size_t offset, size_t len, 
                          uint8_t* ec_space_id, uint8_t* ec_region_type) {
    while (offset < len - 16) {
        if (data[offset] == AML_DEVICE_OP) {
            char name[5];
            size_t new_offset = parse_name(data, offset + 1, name, len - offset - 1);
            
            // Проверяем имя устройства EC
            if (strncmp(name, "EC", 2) == 0 || 
                strncmp(name, "H_EC", 4) == 0 ||
                strncmp(name, "EC0", 3) == 0 ||
                strncmp(name, "EC1", 3) == 0 ||
                strncmp(name, "LID0", 4) == 0) {
                
                term_printf(term, "[AML] Found EC device: %s\n", name);
                
                // Ищем OperationRegion внутри устройства
                for (size_t i = new_offset; i < len - 8 && i < new_offset + 256; i++) {
                    if (data[i] == AML_OPREGION_OP) {
                        // Parse: OpRegionOp, Name, RegionSpace, RegionOffset, RegionLen
                        if (i + 1 < len) {
                            char region_name[5];
                            size_t region_offset = parse_name(data, i + 1, region_name, len - i - 1);
                            
                            if (region_offset + 3 < len) {
                                *ec_space_id = data[region_offset]; // RegionSpace
                                *ec_region_type = data[region_offset]; // Сохраняем тип региона
                                
                                // Регион может быть IO (0x81) или Memory (0x80)
                                term_printf(term, "[AML] EC Region: %s, Space: 0x%02X\n", 
                                           region_name, *ec_space_id);
                                
                                return true;
                            }
                        }
                    }
                }
            }
        }
        offset++;
    }
    
    return false;
}

// Поиск метода _PWR (Power Button)
static bool find_pwr_method(uint8_t* data, size_t offset, size_t len,
                           power_button_info_t* info, uint8_t ec_space_id) {
    while (offset < len - 8) {
        if (data[offset] == AML_METHOD_OP) {
            char name[5];
            size_t new_offset = parse_name(data, offset + 1, name, len - offset - 1);
            
            // Ищем метод _PWR (Power Button)
            if (strncmp(name, "_PWR", 4) == 0) {
                term_printf(term, "[AML] Found _PWR method at offset 0x%X\n", offset);
                
                // Ищем внутри метода операции с EC
                for (size_t i = new_offset + 1; i < len && i < new_offset + 128; i++) {
                    // Ищем операции чтения из EC
                    // 0x70 - Store, 0x72 - RefOf, etc.
                    if (data[i] == 0x70 || data[i] == 0x72 || 
                        data[i] == 0x82 || data[i] == 0x5B) {
                        
                        // Пытаемся найти offset в EC Space
                        // Часто формат: [OpCode][ECSpace][Offset][Value]
                        for (int j = 1; j <= 8 && (i + j) < len; j++) {
                            if (data[i + j] == ec_space_id) {
                                // Следующий байт может быть offset
                                if (i + j + 1 < len) {
                                    info->power_button_offset = data[i + j + 1];
                                    info->power_button_bit = 0x01; // Предполагаем бит 0
                                    info->ec_space_id = ec_space_id;
                                    info->found = true;
                                    
                                    term_printf(term, "[AML] _PWR: EC Space 0x%02X, Offset 0x%02X\n",
                                               ec_space_id, info->power_button_offset);
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        offset++;
    }
    
    return false;
}

// Поиск поля PWRB в Field элементах
static bool find_power_button_field(uint8_t* data, size_t offset, size_t len,
                                   power_button_info_t* info) {
    while (offset < len - 32) {
        // Ищем Field или OperationRegion
        if (data[offset] == AML_FIELD_OP || 
            data[offset] == AML_OPREGION_OP) {
            
            char region_name[5] = {0};
            size_t name_offset = parse_name(data, offset + 1, region_name, len - offset - 1);
            
            // Проверяем, связан ли регион с EC
            if (strncmp(region_name, "EC", 2) == 0 ||
                strncmp(region_name, "EC0", 3) == 0 ||
                strncmp(region_name, "EC1", 3) == 0) {
                
                term_printf(term, "[AML] Found EC region: %s\n", region_name);
                
                // Ищем поле PWRB в этом регионе
                for (size_t i = name_offset; i < len - 8 && i < name_offset + 512; i++) {
                    if (memcmp(&data[i], "PWRB", 4) == 0 ||
                        memcmp(&data[i], "PWRN", 4) == 0 ||
                        memcmp(&data[i], "PBTN", 4) == 0) {
                        
                        term_printf(term, "[AML] Found power button field: %.4s\n", &data[i]);
                        
                        // Формат: [Name (4)][AccessType (1)][AccessLength (1)]
                        if (i + 6 < len) {
                            uint8_t access_type = data[i + 4];
                            uint8_t access_len = data[i + 5];
                            
                            info->power_button_bit = 0x01; // Обычно 1 бит
                            info->found = true;
                            
                            // Пытаемся определить offset из предыдущих Field элементов
                            // Сложно без полного парсера, но можно попробовать
                            term_printf(term, "[AML] Field: AccessType=0x%02X, Length=%d\n",
                                       access_type, access_len);
                            
                            return true;
                        }
                    }
                }
            }
        }
        offset++;
    }
    
    return false;
}

// Поиск Device(PWRB) - отдельное устройство кнопки питания
static bool find_pwr_device(uint8_t* data, size_t offset, size_t len,
                           power_button_info_t* info) {
    while (offset < len - 16) {
        if (data[offset] == AML_DEVICE_OP) {
            char name[5];
            size_t new_offset = parse_name(data, offset + 1, name, len - offset - 1);
            
            // Ищем устройство PWRB, PWRN и т.д.
            if (strncmp(name, "PWRB", 4) == 0 ||
                strncmp(name, "PWRN", 4) == 0 ||
                strncmp(name, "PBTN", 4) == 0) {
                
                term_printf(term, "[AML] Found power button device: %s\n", name);
                
                // Внутри устройства ищем _CRS или _PRS метод
                for (size_t i = new_offset; i < len - 8 && i < new_offset + 256; i++) {
                    if (data[i] == AML_METHOD_OP) {
                        char method_name[5];
                        parse_name(data, i + 1, method_name, len - i - 1);
                        
                        if (strncmp(method_name, "_CRS", 4) == 0 ||
                            strncmp(method_name, "_PRS", 4) == 0) {
                            
                            // В _CRS могут быть ресурсы (IO порты)
                            // Упрощенный парсинг ресурсов
                            for (size_t j = i; j < len - 4 && j < i + 64; j++) {
                                if (data[j] == 0x47) { // IO Resource Descriptor
                                    if (j + 7 < len) {
                                        // Формат: [0x47][Length][IO Flag][Min Addr][Max Addr][Align][Len]
                                        uint16_t io_port = (data[j + 4] << 8) | data[j + 3];
                                        info->power_button_offset = io_port;
                                        info->power_button_region = 0; // IO Space
                                        info->found = true;
                                        
                                        term_printf(term, "[AML] Power button IO port: 0x%04X\n", io_port);
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        offset++;
    }
    
    return false;
}

// Главная функция поиска кнопки питания (многоуровневая)
bool aml_find_power_button(uint8_t* aml_data, size_t aml_len, 
                          power_button_info_t* info) {
    if (!aml_data || aml_len < 64 || !info) {
        return false;
    }
    
    memset(info, 0, sizeof(power_button_info_t));
    
    term_printf(term, "[AML] Searching for power button in %u bytes...\n", aml_len);
    
    uint8_t ec_space_id = 0;
    uint8_t ec_region_type = 0;
    
    // Стратегия 1: Поиск Embedded Controller
    if (find_ec_in_aml(aml_data, 0, aml_len, &ec_space_id, &ec_region_type)) {
        info->ec_space_id = ec_space_id;
        
        // Стратегия 2: Поиск метода _PWR с этим EC
        if (find_pwr_method(aml_data, 0, aml_len, info, ec_space_id)) {
            info->power_button_region = ec_region_type;
            term_printf(term, "[AML] Found via EC+_PWR: Space=0x%02X, Offset=0x%02X\n",
                       info->ec_space_id, info->power_button_offset);
            return true;
        }
        
        // Стратегия 3: Поиск поля PWRB в EC регионе
        if (find_power_button_field(aml_data, 0, aml_len, info)) {
            info->power_button_region = ec_region_type;
            info->ec_space_id = ec_space_id;
            term_printf(term, "[AML] Found via EC field\n");
            return true;
        }
    }
    
    // Стратегия 4: Поиск отдельного устройства PWRB
    if (find_pwr_device(aml_data, 0, aml_len, info)) {
        term_printf(term, "[AML] Found as separate device\n");
        return true;
    }
    
    // Стратегия 5: Поиск по паттернам (fallback)
    term_printf(term, "[AML] Fallback: pattern search...\n");
    
    for (size_t i = 0; i < aml_len - 4; i++) {
        // Ищем паттерны кнопки питания
        if (memcmp(&aml_data[i], "PWRB", 4) == 0 ||
            memcmp(&aml_data[i], "PWRN", 4) == 0 ||
            memcmp(&aml_data[i], "_PWR", 4) == 0 ||
            memcmp(&aml_data[i], "PBTN", 4) == 0) {
            
            term_printf(term, "[AML] Found pattern '%.4s' at 0x%X\n", &aml_data[i], i);
            
            // Эвристика: смотрим на соседние байты
            for (int j = 1; j <= 16 && (i + j) < aml_len; j++) {
                uint8_t val = aml_data[i + j];
                // Ищем байты, похожие на смещения или номера портов
                if ((val >= 0x00 && val <= 0xFF) || 
                    (j < 8 && aml_data[i + j] == 0x70)) { // Store op
                    
                    // Пробуем извлечь offset
                    if (j + 1 < aml_len) {
                        info->power_button_offset = aml_data[i + j + 1];
                        info->power_button_bit = 0x01;
                        info->found = true;
                        info->is_legacy = true;
                        
                        term_printf(term, "[AML] Heuristic offset: 0x%02X\n", 
                                   info->power_button_offset);
                        return true;
                    }
                }
            }
        }
    }
    
    term_printf(term, "[AML] No power button found\n");
    return false;
}

// Инициализация AML контекста
bool aml_init_context(aml_context_t* ctx, acpi_context_t* acpi_ctx) {
    if (!ctx || !acpi_ctx) return false;
    
    memset(ctx, 0, sizeof(aml_context_t));
    
    // Загружаем DSDT
    if (acpi_ctx->dsdt) {
        ctx->dsdt_data = (uint8_t*)acpi_ctx->dsdt;
        ctx->dsdt_len = acpi_ctx->dsdt->header.length;
        term_printf(term, "[AML] DSDT: 0x%llx, %u bytes\n",
                   (uint64_t)(uintptr_t)ctx->dsdt_data, ctx->dsdt_len);
    }
    
    // Загружаем SSDT
    for (int i = 0; i < acpi_ctx->ssdt_count && i < 16; i++) {
        if (acpi_ctx->ssdts[i]) {
            ctx->ssdt_data[i] = (uint8_t*)acpi_ctx->ssdts[i];
            ctx->ssdt_len[i] = acpi_ctx->ssdts[i]->header.length;
            ctx->ssdt_count++;
            
            term_printf(term, "[AML] SSDT[%d]: 0x%llx, %u bytes\n",
                       i, (uint64_t)(uintptr_t)ctx->ssdt_data[i], ctx->ssdt_len[i]);
        }
    }
    
    // Ищем кнопку питания во всех таблицах
    bool found = false;
    
    // Сначала в DSDT
    if (ctx->dsdt_data && ctx->dsdt_len > 64) {
        found = aml_find_power_button(ctx->dsdt_data, ctx->dsdt_len, &ctx->pwr_info);
    }
    
    // Затем в SSDT
    if (!found) {
        for (int i = 0; i < ctx->ssdt_count; i++) {
            if (ctx->ssdt_data[i] && ctx->ssdt_len[i] > 64) {
                found = aml_find_power_button(ctx->ssdt_data[i], ctx->ssdt_len[i], 
                                             &ctx->pwr_info);
                if (found) break;
            }
        }
    }
    
    if (found) {
        term_printf(term, "[AML] Power button info:\n");
        term_printf(term, "  EC Space: 0x%02X\n", ctx->pwr_info.ec_space_id);
        term_printf(term, "  Offset: 0x%04X\n", ctx->pwr_info.power_button_offset);
        term_printf(term, "  Bit: 0x%02X\n", ctx->pwr_info.power_button_bit);
        term_printf(term, "  Region: %d\n", ctx->pwr_info.power_button_region);
    } else {
        term_printf(term, "[AML] Using legacy ACPI power button detection\n");
        ctx->pwr_info.is_legacy = true;
    }
    
    return true;
}

// Получить информацию о кнопке питания
bool aml_get_power_button_info(power_button_info_t* info) {
    if (!info || !g_aml_ctx.pwr_info.found) return false;
    
    memcpy(info, &g_aml_ctx.pwr_info, sizeof(power_button_info_t));
    return true;
}

// Hex дамп для отладки
void aml_hex_dump(uint8_t* data, size_t len, const char* label) {
    if (!term) return;
    
    term_printf(term, "\n[AML] %s (%u bytes):\n", label, len);
    
    size_t limit = (len > 256) ? 256 : len;
    for (size_t i = 0; i < limit; i += 16) {
        term_printf(term, "  %04X: ", i);
        
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                term_printf(term, "%02X ", data[i + j]);
            } else {
                term_printf(term, "   ");
            }
        }
        
        term_printf(term, " ");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[i + j];
            term_printf(term, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        
        term_printf(term, "\n");
    }
}