#include "aml.h"
#include "../../libc/string.h"
#include "../../base/term/tio.h"
#include "../../drv/io/io.h"
#include "../../base/term/term.h"

static aml_context_t g_aml_ctx;
extern term_t* term;

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

/* Пропуск PkgLength */
static size_t aml_skip_pkg_length(uint8_t *data, size_t len, size_t pos) {
    if (pos >= len) return pos;
    
    uint8_t lead = data[pos];
    if (!(lead & 0x80)) return pos + 1; // 1 byte
    
    if ((lead & 0x40) && (lead & 0x1F) == 0) return pos + 2; // 2 bytes
    if ((lead & 0x20) && (lead & 0x0F) == 0) return pos + 3; // 3 bytes
    if ((lead & 0x10) && (lead & 0x07) == 0) return pos + 4; // 4 bytes
    
    return pos + 1; // fallback
}

/* Извлечение PkgLength */
static size_t aml_get_pkg_length(uint8_t *data, size_t len, size_t pos, size_t *new_pos) {
    if (pos >= len) return 0;
    
    uint8_t lead = data[pos];
    *new_pos = pos + 1;
    
    if (!(lead & 0x80)) return lead & 0x3F;
    
    if ((lead & 0x40) && (lead & 0x1F) == 0) {
        if (pos + 1 >= len) return 0;
        *new_pos = pos + 2;
        return ((lead & 0x3F) << 8) | data[pos + 1];
    }
    
    if ((lead & 0x20) && (lead & 0x0F) == 0) {
        if (pos + 2 >= len) return 0;
        *new_pos = pos + 3;
        return ((lead & 0x0F) << 16) | (data[pos + 1] << 8) | data[pos + 2];
    }
    
    if ((lead & 0x10) && (lead & 0x07) == 0) {
        if (pos + 3 >= len) return 0;
        *new_pos = pos + 4;
        return ((lead & 0x07) << 24) | (data[pos + 1] << 16) | 
               (data[pos + 2] << 8) | data[pos + 3];
    }
    
    *new_pos = pos + 1;
    return 0;
}

/* Извлечение NameString */
int aml_extract_namestring(uint8_t *data, size_t len, size_t *pos, char *buf, size_t buf_len) {
    if (!data || !pos || *pos >= len || !buf || buf_len == 0) return -1;
    
    int idx = 0;
    size_t start = *pos;
    
    // Root/Parent префиксы
    while (*pos < len) {
        uint8_t b = data[*pos];
        if (b == 0x5C) { // Root
            if (idx < buf_len - 1) buf[idx++] = '\\';
            (*pos)++;
        } else if (b == 0x5E) { // Parent
            if (idx < buf_len - 1) buf[idx++] = '^';
            (*pos)++;
        } else break;
    }
    
    if (*pos >= len) goto error;
    
    uint8_t lead = data[*pos];
    
    // DualNamePrefix (2 сегмента)
    if (lead == 0x2E) {
        (*pos)++;
        for (int seg = 0; seg < 2; seg++) {
            if (*pos + 4 > len) goto error;
            
            if (seg > 0 && idx < buf_len - 1) buf[idx++] = '.';
            
            for (int i = 0; i < 4; i++) {
                uint8_t c = data[*pos];
                if (c == 0x00) break;
                if (idx < buf_len - 1) buf[idx++] = c;
                (*pos)++;
            }
        }
        buf[idx] = '\0';
        return idx;
    }
    
    // MultiNamePrefix (2F + count)
    if (lead == 0x2F) {
        (*pos)++;
        if (*pos >= len) goto error;
        
        uint8_t count = data[*pos];
        (*pos)++;
        
        for (int seg = 0; seg < count; seg++) {
            if (*pos + 4 > len) goto error;
            
            if (seg > 0 && idx < buf_len - 1) buf[idx++] = '.';
            
            for (int i = 0; i < 4; i++) {
                uint8_t c = data[*pos];
                if (c == 0x00) break;
                if (idx < buf_len - 1) buf[idx++] = c;
                (*pos)++;
            }
        }
        buf[idx] = '\0';
        return idx;
    }
    
    // Single Name (4 байта)
    if (*pos + 4 <= len) {
        for (int i = 0; i < 4; i++) {
            uint8_t c = data[*pos];
            if (c == 0x00) break;
            if (idx < buf_len - 1) buf[idx++] = c;
            (*pos)++;
        }
        buf[idx] = '\0';
        return idx;
    }
    
error:
    *pos = start;
    if (buf_len > 0) buf[0] = '\0';
    return -1;
}

/* Сравнение AML-имён */
bool aml_name_equals(const char *name1, const char *name2) {
    if (!name1 || !name2) return false;
    
    while (*name1 && *name2) {
        if (*name1 == '.' || *name1 == '\\' || *name1 == '^') {
            name1++;
            continue;
        }
        if (*name2 == '.' || *name2 == '\\' || *name2 == '^') {
            name2++;
            continue;
        }
        if (*name1 != *name2) return false;
        name1++;
        name2++;
    }
    return (*name1 == '\0' || *name1 == '.') && 
           (*name2 == '\0' || *name2 == '.');
}

/* Резолв полного пути */
void aml_resolve_path(char *dest, const char *scope, const char *name, size_t max_len) {
    if (!dest || max_len == 0) return;
    
    dest[0] = '\0';
    
    if (!name) return;
    
    // Абсолютный путь
    if (name[0] == '\\') {
        strncpy(dest, name, max_len - 1);
        dest[max_len - 1] = '\0';
        return;
    }
    
    // Относительный путь
    if (scope && scope[0]) {
        strncpy(dest, scope, max_len - 1);
        
        // Убираем trailing dot
        size_t len = strlen(dest);
        if (len > 0 && dest[len - 1] == '.') {
            dest[len - 1] = '\0';
        }
        
        // Добавляем точку если нужно
        if (dest[0] && name[0] && name[0] != '^') {
            strncat(dest, ".", max_len - strlen(dest) - 1);
        }
    }
    
    // Parent префиксы
    const char *n = name;
    while (*n == '^') {
        // Поднимаемся на уровень вверх
        char *last_dot = strrchr(dest, '.');
        if (last_dot) *last_dot = '\0';
        else dest[0] = '\0';
        n++;
    }
    
    // Добавляем имя
    strncat(dest, n, max_len - strlen(dest) - 1);
}

// ==================== ПАРСИНГ КОНКРЕТНЫХ УЗЛОВ ====================

void aml_parse_device(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos) {
    if (!ctx || !data || !pos || *pos >= len) return;
    
    size_t start = *pos;
    (*pos)++; // Skip 0x5B
    
    if (*pos >= len || data[*pos] != 0x82) { // DeviceOp = 0x5B82
        *pos = start;
        return;
    }
    (*pos)++;
    
    // PkgLength
    size_t pkg_end = *pos;
    size_t pkg_len = aml_get_pkg_length(data, len, *pos, &pkg_end);
    if (pkg_len == 0) {
        *pos = start;
        return;
    }
    *pos = pkg_end;
    
    // Device Name
    char device_name[128];
    if (aml_extract_namestring(data, len, pos, device_name, sizeof(device_name)) < 0) {
        *pos = start;
        return;
    }
    
    char full_path[256];
    aml_resolve_path(full_path, ctx->current_scope, device_name, sizeof(full_path));
    
    // Проверяем, является ли устройство кнопкой питания
    if (strstr(full_path, "PWRB") || strstr(full_path, "PWRN") || 
        strstr(full_path, "PBTN") || strstr(full_path, "PWRF")) {
        
        tio_printf("[AML] Found power button device: %s\n", full_path);
        
        ctx->pwr_info.has_pwrb_device = true;
        strncpy(ctx->pwr_info.pwrb_path, full_path, sizeof(ctx->pwr_info.pwrb_path) - 1);
        ctx->pwr_info.pwrb_path[sizeof(ctx->pwr_info.pwrb_path) - 1] = '\0';
        ctx->pwr_info.found = true;
    }
    
    // Заходим в устройство
    char old_scope[256];
    strncpy(old_scope, ctx->current_scope, sizeof(old_scope));
    
    strncpy(ctx->current_scope, full_path, sizeof(ctx->current_scope) - 1);
    ctx->current_scope[sizeof(ctx->current_scope) - 1] = '\0';
    
    // Парсим содержимое устройства
    while (*pos < start + pkg_len) {
        uint8_t op = data[*pos];
        
        if (op == 0x5B) {
            if (*pos + 1 < len) {
                uint8_t ext_op = data[*pos + 1];
                if (ext_op == 0x80) aml_parse_opregion(ctx, data, len, pos);
                else if (ext_op == 0x81) aml_parse_field(ctx, data, len, pos);
                else (*pos)++;
            } else (*pos)++;
        } else if (op == AML_METHOD_OP) {
            aml_parse_method(ctx, data, len, pos);
        } else if (op == AML_SCOPE_OP) {
            aml_parse_scope(ctx, data, len, pos);
        } else if (op == AML_DEVICE_OP) {
            aml_parse_device(ctx, data, len, pos);
        } else {
            (*pos)++;
        }
    }
    
    // Выходим из устройства
    strncpy(ctx->current_scope, old_scope, sizeof(ctx->current_scope));
    
    *pos = start + pkg_len;
}

void aml_parse_scope(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos) {
    if (!ctx || !data || !pos || *pos >= len) return;
    
    size_t start = *pos;
    (*pos)++;
    
    // PkgLength
    size_t pkg_end = *pos;
    size_t pkg_len = aml_get_pkg_length(data, len, *pos, &pkg_end);
    if (pkg_len == 0) {
        *pos = start;
        return;
    }
    *pos = pkg_end;
    
    // Scope Name
    char scope_name[128];
    if (aml_extract_namestring(data, len, pos, scope_name, sizeof(scope_name)) < 0) {
        *pos = start;
        return;
    }
    
    char full_path[256];
    aml_resolve_path(full_path, ctx->current_scope, scope_name, sizeof(full_path));
    
    // Особый интерес представляет Scope(_GPE)
    if (strstr(full_path, "_GPE") || strstr(full_path, "\\_GPE")) {
        tio_printf("[AML] Found GPE scope: %s\n", full_path);
    }
    
    // Заходим в scope
    char old_scope[256];
    strncpy(old_scope, ctx->current_scope, sizeof(old_scope));
    
    strncpy(ctx->current_scope, full_path, sizeof(ctx->current_scope) - 1);
    ctx->current_scope[sizeof(ctx->current_scope) - 1] = '\0';
    
    // Парсим содержимое scope
    while (*pos < start + pkg_len) {
        uint8_t op = data[*pos];
        
        if (op == 0x5B) {
            if (*pos + 1 < len) {
                uint8_t ext_op = data[*pos + 1];
                if (ext_op == 0x80) aml_parse_opregion(ctx, data, len, pos);
                else if (ext_op == 0x81) aml_parse_field(ctx, data, len, pos);
                else if (ext_op == 0x82) aml_parse_device(ctx, data, len, pos);
                else (*pos)++;
            } else (*pos)++;
        } else if (op == AML_METHOD_OP) {
            aml_parse_method(ctx, data, len, pos);
        } else if (op == AML_SCOPE_OP) {
            aml_parse_scope(ctx, data, len, pos);
        } else if (op == AML_DEVICE_OP) {
            aml_parse_device(ctx, data, len, pos);
        } else {
            (*pos)++;
        }
    }
    
    // Выходим
    strncpy(ctx->current_scope, old_scope, sizeof(ctx->current_scope));
    *pos = start + pkg_len;
}

void aml_parse_method(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos) {
    if (!ctx || !data || !pos || *pos >= len) return;
    
    size_t start = *pos;
    (*pos)++;
    
    // PkgLength
    size_t pkg_end = *pos;
    size_t pkg_len = aml_get_pkg_length(data, len, *pos, &pkg_end);
    if (pkg_len == 0) {
        *pos = start;
        return;
    }
    *pos = pkg_end;
    
    // Method Name
    char method_name[128];
    if (aml_extract_namestring(data, len, pos, method_name, sizeof(method_name)) < 0) {
        *pos = start;
        return;
    }
    
    // Method flags
    if (*pos < len) {
        uint8_t flags = data[*pos];
        (*pos)++;
        
        // int arg_count = flags & 0x07;
        // bool serialized = (flags & 0x08) != 0;
    }
    
    char full_path[256];
    aml_resolve_path(full_path, ctx->current_scope, method_name, sizeof(full_path));
    
    // Проверяем GPE-методы (_Lxx, _Exx)
    if (strncmp(method_name, "_L", 2) == 0 || strncmp(method_name, "_E", 2) == 0) {
        int gpe_num = 0;
        if (method_name[2] >= '0' && method_name[2] <= '9') {
            gpe_num = method_name[2] - '0';
            if (method_name[3] >= '0' && method_name[3] <= '9') {
                gpe_num = gpe_num * 10 + (method_name[3] - '0');
            }
        }
        
        tio_printf("[AML] Found GPE method: %s (GPE %d)\n", full_path, gpe_num);
        
        // Ищем внутри метода Notify(PWRB)
        size_t method_body_start = *pos;
        while (*pos < start + pkg_len - 2) {
            if (*pos + 2 < len && data[*pos] == AML_NOTIFY_OP) {
                (*pos)++;
                
                char notify_target[128];
                size_t notify_pos = *pos;
                if (aml_extract_namestring(data, len, &notify_pos, notify_target, sizeof(notify_target)) >= 0) {
                    if (strstr(notify_target, "PWRB") || strstr(notify_target, "PWRN") || strstr(notify_target, "PBTN")) {
                        tio_printf("[AML]   -> Notify(PWRB) found!\n");
                        
                        // Сохраняем GPE информацию
                        aml_gpe_info_t *gpe = &ctx->gpe_info[ctx->gpe_count];
                        memset(gpe, 0, sizeof(aml_gpe_info_t));
                        
                        gpe->gpe_number = gpe_num;
                        gpe->gpe_bit = gpe_num; // Обычно бит == номеру
                        gpe->gpe_type = (method_name[1] == 'E') ? 1 : 0; // 0=_L, 1=_E
                        gpe->found = true;
                        gpe->is_power_button = true;
                        
                        if (ctx->gpe_count < 32) ctx->gpe_count++;
                        
                        // Запоминаем в информации о кнопке
                        ctx->pwr_info.gpe = *gpe;
                        ctx->pwr_info.found = true;
                    }
                }
                break;
            }
            (*pos)++;
        }
    }
    
    *pos = start + pkg_len;
}

void aml_parse_opregion(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos) {
    if (!ctx || !data || !pos || *pos >= len) return;
    
    size_t start = *pos;
    (*pos)++; // Skip 0x5B
    
    if (*pos >= len || data[*pos] != 0x80) { // OpRegionOp
        *pos = start;
        return;
    }
    (*pos)++;
    
    // PkgLength
    size_t pkg_end = *pos;
    size_t pkg_len = aml_get_pkg_length(data, len, *pos, &pkg_end);
    if (pkg_len == 0) {
        *pos = start;
        return;
    }
    *pos = pkg_end;
    
    // Region Name
    char region_name[128];
    if (aml_extract_namestring(data, len, pos, region_name, sizeof(region_name)) < 0) {
        *pos = start;
        return;
    }
    
    // RegionSpace
    if (*pos >= len) {
        *pos = start;
        return;
    }
    uint8_t region_space = data[*pos];
    (*pos)++;
    
    // RegionOffset (Term)
    // Реальный парсинг Term сложен, для упрощения ищем константы
    uint64_t region_offset = 0;
    if (*pos < len) {
        if (data[*pos] == AML_BYTE_PREFIX && *pos + 2 < len) {
            region_offset = data[*pos + 1];
            *pos += 2;
        } else if (data[*pos] == AML_WORD_PREFIX && *pos + 3 < len) {
            region_offset = data[*pos + 1] | (data[*pos + 2] << 8);
            *pos += 3;
        } else if (data[*pos] == AML_DWORD_PREFIX && *pos + 5 < len) {
            region_offset = data[*pos + 1] | (data[*pos + 2] << 8) |
                           (data[*pos + 3] << 16) | (data[*pos + 4] << 24);
            *pos += 5;
        }
    }
    
    // RegionLen (Term)
    uint64_t region_len = 0;
    if (*pos < len) {
        if (data[*pos] == AML_BYTE_PREFIX && *pos + 2 < len) {
            region_len = data[*pos + 1];
            *pos += 2;
        } else if (data[*pos] == AML_WORD_PREFIX && *pos + 3 < len) {
            region_len = data[*pos + 1] | (data[*pos + 2] << 8);
            *pos += 3;
        } else if (data[*pos] == AML_DWORD_PREFIX && *pos + 5 < len) {
            region_len = data[*pos + 1] | (data[*pos + 2] << 8) |
                        (data[*pos + 3] << 16) | (data[*pos + 4] << 24);
            *pos += 5;
        }
    }
    
    // Интересует только EC
    if (region_space == AML_REGION_EMBEDDED_CTRL) {
        tio_printf("[AML] Found EC operation region: %s at 0x%lx (len 0x%lx)\n",
                   region_name, region_offset, region_len);
        
        ctx->pwr_info.ec_space_id = region_space;
        ctx->pwr_info.power_button_region = region_space;
    }
    
    *pos = start + pkg_len;
}

void aml_parse_field(aml_context_t *ctx, uint8_t *data, size_t len, size_t *pos) {
    if (!ctx || !data || !pos || *pos >= len) return;
    
    size_t start = *pos;
    (*pos)++; // Skip 0x5B
    
    if (*pos >= len || data[*pos] != 0x81) { // FieldOp
        *pos = start;
        return;
    }
    (*pos)++;
    
    // PkgLength
    size_t pkg_end = *pos;
    size_t pkg_len = aml_get_pkg_length(data, len, *pos, &pkg_end);
    if (pkg_len == 0) {
        *pos = start;
        return;
    }
    *pos = pkg_end;
    
    // Field Name (operation region reference)
    char field_name[128];
    if (aml_extract_namestring(data, len, pos, field_name, sizeof(field_name)) < 0) {
        *pos = start;
        return;
    }
    
    // FieldFlags
    if (*pos >= len) {
        *pos = start;
        return;
    }
    uint8_t field_flags = data[*pos];
    (*pos)++;
    
    // Field list
    while (*pos < start + pkg_len - 1) {
        uint8_t field_op = data[*pos];
        
        if (field_op == 0x00) { // ReservedField
            (*pos)++;
            if (*pos >= len) break;
            uint8_t length = data[*pos];
            (*pos)++;
        } else if (field_op == 0x01) { // AccessField
            (*pos)++;
            if (*pos + 1 >= len) break;
            uint8_t type = data[*pos];
            (*pos)++;
            uint8_t attrib = data[*pos];
            (*pos)++;
        } else if (field_op == 0x02) { // ConnectField
            (*pos)++;
            // Сложно, пропускаем
        } else {
            // NamedField
            char field_entry_name[128];
            size_t name_pos = *pos;
            if (aml_extract_namestring(data, len, &name_pos, field_entry_name, sizeof(field_entry_name)) >= 0) {
                *pos = name_pos;
                
                if (*pos + 1 < len) {
                    uint8_t field_length = data[*pos];
                    (*pos)++;
                    
                    // Проверяем, не является ли это полем кнопки питания
                    if (strstr(field_entry_name, "PWRB") || strstr(field_entry_name, "PWRN") ||
                        strstr(field_entry_name, "PBTN") || strstr(field_entry_name, "PWRF")) {
                        
                        tio_printf("[AML] Found power button field: %s (bit length: %d)\n",
                                   field_entry_name, field_length);
                        
                        // Пытаемся вычислить offset в байтах
                        ctx->pwr_info.power_button_bit = 0; // Обычно бит 0
                        ctx->pwr_info.found = true;
                    }
                }
            } else {
                (*pos)++;
            }
        }
    }
    
    *pos = start + pkg_len;
}

// ==================== ОБХОД AML ====================

void aml_walk_scope(aml_context_t *ctx, uint8_t *data, size_t len, int depth) {
    if (!ctx || !data || len == 0) return;
    
    size_t pos = 0;
    
    while (pos < len) {
        if (pos + 1 > len) break;
        
        uint8_t op = data[pos];
        
        if (op == 0x5B) { // Extended opcode
            if (pos + 1 < len) {
                uint8_t ext_op = data[pos + 1];
                
                if (ext_op == 0x80) { // OperationRegion
                    aml_parse_opregion(ctx, data, len, &pos);
                } else if (ext_op == 0x81) { // Field
                    aml_parse_field(ctx, data, len, &pos);
                } else if (ext_op == 0x82) { // Device
                    aml_parse_device(ctx, data, len, &pos);
                } else if (ext_op == 0x85) { // ThermalZone
                    size_t start = pos;
                    pos += 2;
                    size_t pkg_end = pos;
                    size_t pkg_len = aml_get_pkg_length(data, len, pos, &pkg_end);
                    if (pkg_len > 0) pos = start + pkg_len;
                    else pos = start + 1;
                } else {
                    pos += 2;
                }
            } else pos++;
        } else if (op == AML_SCOPE_OP) {
            aml_parse_scope(ctx, data, len, &pos);
        } else if (op == AML_DEVICE_OP) {
            aml_parse_device(ctx, data, len, &pos);
        } else if (op == AML_METHOD_OP) {
            aml_parse_method(ctx, data, len, &pos);
        } else if (op == AML_PROCESSOR_OP) {
            pos += 2;
        } else if (op == AML_NAME_OP) {
            pos++;
            char name[128];
            aml_extract_namestring(data, len, &pos, name, sizeof(name));
        } else {
            pos++;
        }
    }
}

void aml_walk_tables(aml_context_t *ctx) {
    if (!ctx) return;
    
    // DSDT
    if (ctx->dsdt_data && ctx->dsdt_len > 64) {
        tio_printf("[AML] Walking DSDT...\n");
        strcpy(ctx->current_scope, "\\");
        aml_walk_scope(ctx, ctx->dsdt_data, ctx->dsdt_len, 0);
    }
    
    // SSDT
    for (int i = 0; i < ctx->ssdt_count; i++) {
        if (ctx->ssdt_data[i] && ctx->ssdt_len[i] > 64) {
            tio_printf("[AML] Walking SSDT[%d]...\n", i);
            strcpy(ctx->current_scope, "\\");
            aml_walk_scope(ctx, ctx->ssdt_data[i], ctx->ssdt_len[i], 0);
        }
    }
}

// ==================== ПУБЛИЧНЫЕ ФУНКЦИИ ====================

bool aml_init_context(aml_context_t *ctx, acpi_context_t *acpi_ctx) {
    if (!ctx || !acpi_ctx) return false;
    
    memset(ctx, 0, sizeof(aml_context_t));
    
    // DSDT
    if (acpi_ctx->dsdt) {
        ctx->dsdt_data = (uint8_t*)acpi_ctx->dsdt;
        ctx->dsdt_len = acpi_ctx->dsdt->header.length;
        tio_printf("[AML] DSDT: 0x%lx, %lu bytes\n",
                   (uint64_t)(uintptr_t)ctx->dsdt_data, ctx->dsdt_len);
    }
    
    // SSDT
    for (int i = 0; i < acpi_ctx->ssdt_count && i < 16; i++) {
        if (acpi_ctx->ssdts[i]) {
            ctx->ssdt_data[i] = (uint8_t*)acpi_ctx->ssdts[i];
            ctx->ssdt_len[i] = acpi_ctx->ssdts[i]->header.length;
            ctx->ssdt_count++;
            
            tio_printf("[AML] SSDT[%d]: 0x%lx, %lu bytes\n",
                       i, (uint64_t)(uintptr_t)ctx->ssdt_data[i], ctx->ssdt_len[i]);
        }
    }
    
    // Ищем кнопку питания
    ctx->initialized = true;
    aml_find_power_button(ctx);
    
    return true;
}

bool aml_find_power_button(aml_context_t *ctx) {
    if (!ctx || !ctx->initialized) return false;
    
    tio_printf("\n[AML] Searching for power button...\n");
    
    memset(&ctx->pwr_info, 0, sizeof(power_button_info_t));
    ctx->gpe_count = 0;
    
    // Ставим fallback
    ctx->pwr_info.pm1_evt_blk = 0;
    ctx->pwr_info.pm1_pwr_btn_bit = 8;
    ctx->pwr_info.is_legacy = true;
    
    // Обходим все таблицы
    aml_walk_tables(ctx);
    
    // Если нашли GPE с кнопкой — отлично
    if (ctx->pwr_info.gpe.found) {
        tio_printf("[AML] Power button found via GPE %d\n",
                   ctx->pwr_info.gpe.gpe_number);
        ctx->pwr_info.found = true;
        ctx->pwr_info.is_legacy = false;
        return true;
    }
    
    // Если нашли PWRB устройство — хорошо
    if (ctx->pwr_info.has_pwrb_device) {
        tio_printf("[AML] Power button found via PWRB device\n");
        ctx->pwr_info.found = true;
        ctx->pwr_info.is_legacy = false;
        return true;
    }
    
    // Если нашли EC field — тоже неплохо
    if (ctx->pwr_info.power_button_offset != 0) {
        tio_printf("[AML] Power button found via EC field at 0x%02X\n",
                   ctx->pwr_info.power_button_offset);
        ctx->pwr_info.found = true;
        ctx->pwr_info.is_legacy = false;
        return true;
    }
    
    tio_printf("[AML] No power button found, using legacy PM1 method\n");
    return false;
}

bool aml_get_power_button_info(power_button_info_t *info) {
    if (!info) return false;
    
    memcpy(info, &g_aml_ctx.pwr_info, sizeof(power_button_info_t));
    return info->found || info->is_legacy;
}

bool aml_get_gpe_info(aml_gpe_info_t *info, int index) {
    if (!info || index < 0 || index >= g_aml_ctx.gpe_count) return false;
    
    memcpy(info, &g_aml_ctx.gpe_info[index], sizeof(aml_gpe_info_t));
    return true;
}

void aml_print_info(aml_context_t *ctx) {
    if (!ctx || !term) return;
    
    tio_printf("\n=== AML Parser Information ===\n");
    
    if (ctx->pwr_info.found) {
        tio_printf("Power button: FOUND\n");
        
        if (ctx->pwr_info.gpe.found) {
            tio_printf("  Method: GPE %d (%s)\n",
                       ctx->pwr_info.gpe.gpe_number,
                       ctx->pwr_info.gpe.gpe_type ? "_E" : "_L");
        }
        
        if (ctx->pwr_info.has_pwrb_device) {
            tio_printf("  Device: %s\n", ctx->pwr_info.pwrb_path);
        }
        
        if (ctx->pwr_info.power_button_offset != 0) {
            tio_printf("  EC: offset 0x%02X, bit %d\n",
                       ctx->pwr_info.power_button_offset,
                       ctx->pwr_info.power_button_bit);
        }
    } else {
        tio_printf("Power button: NOT FOUND (using legacy)\n");
    }
    
    tio_printf("GPE handlers: %d\n", ctx->gpe_count);
    
    for (int i = 0; i < ctx->gpe_count && i < 5; i++) {
        tio_printf("  GPE[%d]: %s GPE%d\n", i,
                   ctx->gpe_info[i].gpe_type ? "_E" : "_L",
                   ctx->gpe_info[i].gpe_number);
    }
    
    tio_printf("===============================\n\n");
}

void aml_hex_dump(uint8_t *data, size_t len, const char *label) {
    if (!term || !data || len == 0) return;
    
    tio_printf("\n[AML] %s (%lu bytes):\n", label, len);
    
    size_t limit = (len > 256) ? 256 : len;
    for (size_t i = 0; i < limit; i += 16) {
        tio_printf("  %04lX: ", i);
        
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) tio_printf("%02X ", data[i + j]);
            else tio_printf("   ");
        }
        
        tio_printf(" ");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[i + j];
            tio_printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        
        tio_printf("\n");
    }
}
