// drv/usb/ehci.c
#include "ehci.h"
#include "../pci/pci.h"
#include "../io/io.h"
#include "../../base/mem/mem.h"
#include "../../libc/string.h"
#include "../../base/int/idt.h"
#include "../pic/pic.h"
#include <stddef.h>

static ehci_controller_t main_controller;
extern term_t* term;

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

// Чтение/запись MMIO
static uint32_t ehci_read(ehci_controller_t* ctrl, uint32_t offset) {
    return mmio_read32((void*)((uintptr_t)ctrl->mmio_base + offset));
}

static void ehci_write(ehci_controller_t* ctrl, uint32_t offset, uint32_t value) {
    mmio_write32((void*)((uintptr_t)ctrl->mmio_base + offset), value);
}

// Выделение выровненной памяти для структур
static void* ehci_alloc_aligned(size_t size, size_t align) {
    void* ptr = malloc(size + align - 1 + sizeof(void*));
    if (!ptr) return NULL;
    
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned = (addr + sizeof(void*) + align - 1) & ~(align - 1);
    
    // Сохраняем оригинальный указатель перед выровненным блоком
    *((void**)(aligned - sizeof(void*))) = ptr;
    
    return (void*)aligned;
}

static void ehci_free_aligned(void* aligned) {
    if (!aligned) return;
    void* original = *((void**)((uintptr_t)aligned - sizeof(void*)));
    free(original);
}

// Создание QH (Queue Head)
static ehci_qh_t* ehci_create_qh(void) {
    ehci_qh_t* qh = (ehci_qh_t*)ehci_alloc_aligned(sizeof(ehci_qh_t), 32);
    if (!qh) return NULL;
    
    memset(qh, 0, sizeof(ehci_qh_t));
    qh->horiz_link_ptr = 1; // Терминатор
    qh->char_bytes = (1 << 15); // H = 1 (Head)
    qh->qtd_overlay.next_qtd = 1; // Терминатор
    
    return qh;
}

// Создание QTD (Queue Transfer Descriptor)
static ehci_qtd_t* ehci_create_qtd(void) {
    ehci_qtd_t* qtd = (ehci_qtd_t*)ehci_alloc_aligned(sizeof(ehci_qtd_t), 32);
    if (!qtd) return NULL;
    
    memset(qtd, 0, sizeof(ehci_qtd_t));
    qtd->next_qtd = 1; // Терминатор
    qtd->alt_next_qtd = 1; // Терминатор
    
    return qtd;
}

// Ожидание завершения передачи
static bool ehci_wait_qtd(ehci_qtd_t* qtd, uint32_t timeout_ms) {
    for (uint32_t i = 0; i < timeout_ms * 1000; i++) {
        uint32_t token = qtd->token;
        if (token & (1 << 7)) { // Active bit cleared
            if (token & (1 << 8)) { // Halted
                return false;
            }
            if (token & (1 << 9)) { // Data buffer error
                return false;
            }
            if (token & (1 << 10)) { // Babble detected
                return false;
            }
            if (token & (1 << 11)) { // Transaction error
                return false;
            }
            if (token & (1 << 12)) { // Missed micro-frame
                return false;
            }
            if (token & (1 << 13)) { // Split transaction state
                return false;
            }
            if (token & (1 << 14)) { // Ping state
                return false;
            }
            if (token & (1 << 15)) { // Error counter > 0
                return false;
            }
            return true; // Transfer completed successfully
        }
        for (volatile int j = 0; j < 1000; j++);
    }
    return false; // Timeout
}

// ==================== ОСНОВНЫЕ ФУНКЦИИ ====================

// Обнаружение контроллера EHCI
bool ehci_detect_controller(term_t* term) {
    term_printf(term, "[EHCI] Scanning for USB 2.0 controller...\n");
    
    // Ищем контроллер по классу/подклассу (PCI Class 0x0C, Subclass 0x03, ProgIF 0x20)
    pci_device_t* pci_dev = pci_find_class(0x0C, 0x03);
    if (!pci_dev) {
        term_printf(term, "[EHCI] No USB controller found\n");
        return false;
    }
    
    // Проверяем, что это EHCI (ProgIF = 0x20)
    uint32_t class_rev = pci_read(pci_dev, 0x08);
    uint8_t prog_if = (class_rev >> 8) & 0xFF;

    uint16_t vendor = pci_dev->vendor_id;
    uint16_t device = pci_dev->device_id;

    if (vendor == 0x8086) { // Intel
        // Intel ICH9 USB2 (EHCI) может иметь ProgIF=0x10
        switch (device) {
            case 0x293A: // ICH9
            case 0x293C: // ICH9
            case 0x3A34: // ICH10
            case 0x3B34: // 5 Series
                term_printf(term, "[USB] Intel EHCI detected by Device ID\n");
                return true;
        }
    }
    
    if (prog_if != 0x20) {
        term_printf(term, "[EHCI] Found USB controller but not EHCI (ProgIF: 0x%02X)\n", prog_if);
        free(pci_dev);
        return false;
    }
    
    term_printf(term, "[EHCI] Found controller at %02X:%02X.%X\n",
                pci_dev->bus, pci_dev->slot, pci_dev->function);
    
    // Включаем устройство
    pci_enable(pci_dev);
    pci_enable_busmaster(pci_dev);
    
    // Получаем BAR (должен быть BAR0)
    uint64_t bar = pci_dev->bars[0];
    if (bar == 0 || (bar & 1)) {
        term_printf(term, "[EHCI] Invalid BAR0\n");
        free(pci_dev);
        return false;
    }
    
    bar &= ~1ULL;
    
    // Инициализируем структуру контроллера
    memset(&main_controller, 0, sizeof(ehci_controller_t));
    main_controller.mmio_base = bar;
    main_controller.cap_regs = (ehci_cap_regs_t*)bar;
    main_controller.op_regs = (ehci_op_regs_t*)(bar + main_controller.cap_regs->cap_length);
    
    // Получаем количество портов
    uint32_t hcs_params = main_controller.cap_regs->hcs_params;
    main_controller.num_ports = (hcs_params >> 0) & 0x0F;
    main_controller.num_companion = (hcs_params >> 8) & 0x0F;
    
    // Получаем IRQ
    uint32_t int_line = pci_read(pci_dev, 0x3C);
    main_controller.irq_line = int_line & 0xFF;
    
    term_printf(term, "[EHCI] Controller at 0x%llx, Ports: %d, Companion: %d, IRQ: %d\n",
                bar, main_controller.num_ports, main_controller.num_companion,
                main_controller.irq_line);
    
    free(pci_dev);
    return true;
}

// Сброс контроллера
bool ehci_reset_controller(ehci_controller_t* ctrl) {
    if (!ctrl) return false;
    
    term_printf(term, "[EHCI] Resetting controller...\n");
    
    // 1. Останавливаем контроллер
    ehci_write(ctrl, main_controller.cap_regs->cap_length + EHCI_USBCMD_OFFSET, 0);
    
    // 2. Ждем остановки
    uint32_t timeout = 10000;
    while (timeout--) {
        uint32_t usbsts = ehci_read(ctrl, main_controller.cap_regs->cap_length + EHCI_USBSTS_OFFSET);
        if (!(usbsts & (1 << 0))) break; // HCHalted
        for (volatile int i = 0; i < 1000; i++);
    }
    
    // 3. Сбрасываем контроллер
    ehci_write(ctrl, main_controller.cap_regs->cap_length + EHCI_USBCMD_OFFSET, (1 << 1));
    
    // 4. Ждем сброса
    timeout = 10000;
    while (timeout--) {
        uint32_t usbcmd = ehci_read(ctrl, main_controller.cap_regs->cap_length + EHCI_USBCMD_OFFSET);
        if (!(usbcmd & (1 << 1))) break; // Reset bit cleared
        for (volatile int i = 0; i < 1000; i++);
    }
    
    // 5. Очищаем все прерывания
    ehci_write(ctrl, main_controller.cap_regs->cap_length + EHCI_USBSTS_OFFSET, 0x3F);
    
    return true;
}

// Запуск контроллера
bool ehci_start_controller(ehci_controller_t* ctrl) {
    if (!ctrl) return false;
    
    term_printf(term, "[EHCI] Starting controller...\n");
    
    // 1. Выделяем Frame List
    ctrl->frame_list_size = 1024; // 1K entries
    ctrl->frame_list = (uint32_t*)ehci_alloc_aligned(ctrl->frame_list_size * sizeof(uint32_t), 4096);
    if (!ctrl->frame_list) return false;
    
    // Инициализируем Frame List как терминаторы
    for (uint16_t i = 0; i < ctrl->frame_list_size; i++) {
        ctrl->frame_list[i] = 1; // Терминатор
    }
    
    // 2. Создаем Async и Periodic списки
    ctrl->async_qh = ehci_create_qh();
    ctrl->periodic_qh = ehci_create_qh();
    if (!ctrl->async_qh || !ctrl->periodic_qh) {
        return false;
    }
    
    // 3. Настраиваем регистры
    uint32_t cap_len = ctrl->cap_regs->cap_length;
    
    // Frame List Base Address
    uint32_t frame_list_phys = (uint32_t)(uintptr_t)ctrl->frame_list;
    ehci_write(ctrl, cap_len + EHCI_PERIODICLIST_OFFSET, frame_list_phys);
    
    // Async List Address
    uint32_t async_qh_phys = (uint32_t)(uintptr_t)ctrl->async_qh;
    ehci_write(ctrl, cap_len + EHCI_ASYNCLIST_OFFSET, async_qh_phys);
    
    // Устанавливаем бит Run/Stop
    uint32_t usbcmd = ehci_read(ctrl, cap_len + EHCI_USBCMD_OFFSET);
    usbcmd |= (1 << 0); // Run
    usbcmd |= (1 << 4); // Async schedule enable
    usbcmd |= (1 << 3); // Periodic schedule enable
    usbcmd &= ~(1 << 2); // Frame list size = 1024
    ehci_write(ctrl, cap_len + EHCI_USBCMD_OFFSET, usbcmd);
    
    // 4. Устанавливаем бит собственности (отключаем companion controllers)
    if (ctrl->num_companion > 0) {
        ehci_write(ctrl, cap_len + EHCI_CONFIGFLAG_OFFSET, (1 << 0));
    }
    
    // 5. Включаем прерывания
    ehci_write(ctrl, cap_len + EHCI_USBINTR_OFFSET,
               (1 << 0) |  // USB interrupt enable
               (1 << 1) |  // USB error interrupt enable
               (1 << 2) |  // Port change interrupt enable
               (1 << 3) |  // Frame list rollover enable
               (1 << 4));  // Host system error enable
    
    ctrl->initialized = true;
    term_printf(term, "[EHCI] Controller started successfully\n");
    return true;
}

// Сброс порта
bool ehci_port_reset(ehci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || port >= ctrl->num_ports) return false;
    
    term_printf(term, "[EHCI] Resetting port %d...\n", port);
    
    uint32_t portsc = ehci_read(ctrl, main_controller.cap_regs->cap_length + 
                                EHCI_PORTSC_OFFSET + (port * 4));
    
    // Запускаем сброс
    portsc &= ~(0xFF << 16); // Clear port speed
    portsc |= (1 << 8);      // Port reset
    
    ehci_write(ctrl, main_controller.cap_regs->cap_length + 
               EHCI_PORTSC_OFFSET + (port * 4), portsc);
    
    // Ждем 20ms (USB spec)
    for (volatile int i = 0; i < 20000; i++);
    
    // Останавливаем сброс
    portsc &= ~(1 << 8);
    ehci_write(ctrl, main_controller.cap_regs->cap_length + 
               EHCI_PORTSC_OFFSET + (port * 4), portsc);
    
    // Ждем 10ms для стабилизации
    for (volatile int i = 0; i < 10000; i++);
    
    // Проверяем статус
    portsc = ehci_read(ctrl, main_controller.cap_regs->cap_length + 
                       EHCI_PORTSC_OFFSET + (port * 4));
    
    if ((portsc & (1 << 1)) &&  // Connect status
        (portsc & (1 << 2)) &&  // Port enabled
        !(portsc & (1 << 4))) { // Port overcurrent not active
        term_printf(term, "[EHCI] Port %d reset successful\n", port);
        return true;
    }
    
    term_printf(term, "[EHCI] Port %d reset failed (status: 0x%08X)\n", port, portsc);
    return false;
}

// Перечисление устройства
usb_device_t* ehci_enumerate_device(ehci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || port >= ctrl->num_ports || ctrl->device_count >= 127) {
        return NULL;
    }
    
    term_printf(term, "[EHCI] Enumerating device on port %d...\n", port);
    
    usb_device_t* dev = &ctrl->devices[ctrl->device_count];
    memset(dev, 0, sizeof(usb_device_t));
    
    // Читаем порт статус
    uint32_t portsc = ehci_read(ctrl, main_controller.cap_regs->cap_length + 
                                EHCI_PORTSC_OFFSET + (port * 4));
    
    // Определяем скорость
    uint8_t port_speed = (portsc >> 26) & 0x03;
    dev->speed = port_speed;
    dev->port = port;
    
    // Устанавливаем адрес устройства
    dev->address = ctrl->device_count + 1; // Адрес 0 зарезервирован
    
    // Определяем размер пакета по умолчанию
    if (dev->speed == 0) { // Low speed
        dev->max_packet_size = 8;
    } else if (dev->speed == 1) { // Full speed
        dev->max_packet_size = 8; // Будет обновлен из дескриптора
    } else { // High speed
        dev->max_packet_size = 64;
    }
    
    // Отправляем запрос SET_ADDRESS
    uint8_t setup_packet[8] = {
        0x00,       // bmRequestType (host->device, стандартный, устройство)
        0x05,       // bRequest = SET_ADDRESS
        dev->address, 0x00, // wValue = адрес
        0x00, 0x00, // wIndex = 0
        0x00, 0x00  // wLength = 0
    };
    
    if (!ehci_control_transfer(dev, 0x00, 0x05, dev->address, 0, 0, NULL)) {
        term_printf(term, "[EHCI] SET_ADDRESS failed for device on port %d\n", port);
        return NULL;
    }
    
    // Ждем установки адреса
    for (volatile int i = 0; i < 10000; i++);
    
    // Получаем дескриптор устройства
    uint8_t device_descriptor[18];
    if (!ehci_control_transfer(dev, 0x80, 0x06, 0x0100, 0, 8, device_descriptor)) {
        term_printf(term, "[EHCI] Failed to get device descriptor\n");
        return NULL;
    }
    
    // Сохраняем информацию
    dev->max_packet_size = device_descriptor[7];
    dev->device_class = device_descriptor[4];
    dev->device_subclass = device_descriptor[5];
    dev->device_protocol = device_descriptor[6];
    dev->vendor_id = device_descriptor[8] | (device_descriptor[9] << 8);
    dev->product_id = device_descriptor[10] | (device_descriptor[11] << 8);
    
    // Устанавливаем конфигурацию
    dev->configuration_value = 1;
    if (!ehci_control_transfer(dev, 0x00, 0x09, 1, 0, 0, NULL)) {
        term_printf(term, "[EHCI] SET_CONFIGURATION failed\n");
        return NULL;
    }
    
    dev->connected = true;
    ctrl->device_count++;
    
    term_printf(term, "[EHCI] Device enumerated: VID:PID %04X:%04X, Class:%d, Speed:%s\n",
                dev->vendor_id, dev->product_id, dev->device_class,
                dev->speed == 0 ? "Low" : dev->speed == 1 ? "Full" : "High");
    
    return dev;
}

// Контрольный трансфер
bool ehci_control_transfer(usb_device_t* dev, uint8_t request_type,
                           uint8_t request, uint16_t value, 
                           uint16_t index, uint16_t length,
                           void* data) {
    if (!dev || !main_controller.initialized) return false;
    
    // 1. Создаем QH для контрольной передачи
    ehci_qh_t* qh = ehci_create_qh();
    if (!qh) return false;
    
    // Настраиваем QH
    qh->char_bytes = (1 << 15) | // H = 1 (Head)
                     (dev->speed << 12) | // EPS = speed
                     (0 << 8) | // Endpoint number = 0 (control)
                     (0 << 7) | // Endpoint direction = OUT
                     (3 << 0);  // Control endpoint type
    
    // Настраиваем максимальный размер пакета
    qh->char_bytes |= (dev->max_packet_size << 16);
    
    // 2. Создаем QTD для SETUP этапа
    ehci_qtd_t* setup_qtd = ehci_create_qtd();
    if (!setup_qtd) {
        ehci_free_aligned(qh);
        return false;
    }
    
    // Создаем SETUP пакет
    uint8_t setup_packet[8] = {
        request_type,
        request,
        value & 0xFF, (value >> 8) & 0xFF,
        index & 0xFF, (index >> 8) & 0xFF,
        length & 0xFF, (length >> 8) & 0xFF
    };
    
    // Настраиваем QTD для SETUP
    setup_qtd->token = (8 << 16) | // Total bytes = 8
                       (1 << 15) | // IOC = 1 (Interrupt On Complete)
                       (0 << 10) | // C_PAGE = 0
                       (0 << 8) | // PID = SETUP
                       (1 << 7);  // Active = 1
    
    setup_qtd->buffer_ptr[0] = (uint32_t)(uintptr_t)setup_packet;
    
    // 3. Создаем QTD для DATA этапа (если есть данные)
    ehci_qtd_t* data_qtd = NULL;
    if (length > 0 && data) {
        data_qtd = ehci_create_qtd();
        if (!data_qtd) {
            ehci_free_aligned(setup_qtd);
            ehci_free_aligned(qh);
            return false;
        }
        
        // Определяем направление
        uint8_t pid = (request_type & 0x80) ? 1 : 0; // 1 = IN, 0 = OUT
        
        // Для IN трансфера нужно указать количество пакетов
        uint8_t packets = (length + dev->max_packet_size - 1) / dev->max_packet_size;
        
        data_qtd->token = (length << 16) | // Total bytes
                          (1 << 15) | // IOC
                          (0 << 10) | // C_PAGE
                          (pid << 8) | // PID (IN или OUT)
                          (1 << 7); // Active
        
        // Для OUT трансфера копируем данные
        if (pid == 0) {
            uint8_t* data_buffer = (uint8_t*)ehci_alloc_aligned(length, 4096);
            if (!data_buffer) {
                ehci_free_aligned(data_qtd);
                ehci_free_aligned(setup_qtd);
                ehci_free_aligned(qh);
                return false;
            }
            memcpy(data_buffer, data, length);
            data_qtd->buffer_ptr[0] = (uint32_t)(uintptr_t)data_buffer;
        } else {
            data_qtd->buffer_ptr[0] = (uint32_t)(uintptr_t)data;
        }
        
        setup_qtd->next_qtd = (uint32_t)(uintptr_t)data_qtd;
    }
    
    // 4. Создаем QTD для STATUS этапа
    ehci_qtd_t* status_qtd = ehci_create_qtd();
    if (!status_qtd) {
        if (data_qtd) ehci_free_aligned(data_qtd);
        ehci_free_aligned(setup_qtd);
        ehci_free_aligned(qh);
        return false;
    }
    
    // STATUS этап всегда противоположен DATA этапу
    uint8_t status_pid = (request_type & 0x80) ? 0 : 1; // 0 = OUT, 1 = IN
    
    status_qtd->token = (0 << 16) | // Total bytes = 0
                        (1 << 15) | // IOC
                        (0 << 10) | // C_PAGE
                        (status_pid << 8) | // PID
                        (1 << 7); // Active
    
    // Связываем QTD
    if (data_qtd) {
        data_qtd->next_qtd = (uint32_t)(uintptr_t)status_qtd;
    } else {
        setup_qtd->next_qtd = (uint32_t)(uintptr_t)status_qtd;
    }
    
    // 5. Помещаем setup_qtd в QH
    qh->qtd_overlay.next_qtd = (uint32_t)(uintptr_t)setup_qtd;
    
    // 6. Добавляем QH в асинхронный список
    ehci_qh_t* async_qh = main_controller.async_qh;
    uint32_t old_horiz = async_qh->horiz_link_ptr;
    async_qh->horiz_link_ptr = (uint32_t)(uintptr_t)qh;
    qh->horiz_link_ptr = old_horiz;
    
    // 7. Ждем завершения
    bool success = ehci_wait_qtd(status_qtd, 100); // 100ms timeout
    
    // 8. Для IN трансфера копируем данные
    if (success && length > 0 && data && (request_type & 0x80)) {
        memcpy(data, (void*)(uintptr_t)data_qtd->buffer_ptr[0], length);
    }
    
    // 9. Очищаем
    if (data_qtd && !(request_type & 0x80)) {
        ehci_free_aligned((void*)(uintptr_t)data_qtd->buffer_ptr[0]);
    }
    ehci_free_aligned(status_qtd);
    if (data_qtd) ehci_free_aligned(data_qtd);
    ehci_free_aligned(setup_qtd);
    
    // Убираем QH из списка
    async_qh->horiz_link_ptr = old_horiz;
    ehci_free_aligned(qh);
    
    return success;
}

// Обработчик прерывания
void ehci_irq_handler(void) {
    ehci_controller_t* ctrl = &main_controller;
    if (!ctrl->initialized) {
        pic_send_eoi(ctrl->irq_line);
        return;
    }
    
    uint32_t usbsts = ehci_read(ctrl, ctrl->cap_regs->cap_length + EHCI_USBSTS_OFFSET);
    
    // Проверяем изменения портов
    if (usbsts & (1 << 2)) {
        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            uint32_t portsc = ehci_read(ctrl, ctrl->cap_regs->cap_length + 
                                       EHCI_PORTSC_OFFSET + (port * 4));
            
            // Проверяем изменение подключения
            if (portsc & (1 << 0)) {
                // Сбросить бит изменения
                ehci_write(ctrl, ctrl->cap_regs->cap_length + 
                          EHCI_PORTSC_OFFSET + (port * 4), portsc | (1 << 0));
                
                // Если устройство подключено
                if (portsc & (1 << 1)) {
                    term_printf(term, "[EHCI] Device connected on port %d\n", port);
                    
                    // Сбрасываем порт
                    if (ehci_port_reset(ctrl, port)) {
                        // Перечисляем устройство
                        ehci_enumerate_device(ctrl, port);
                    }
                } else {
                    term_printf(term, "[EHCI] Device disconnected on port %d\n", port);
                }
            }
        }
    }
    
    // Сбрасываем все флаги прерываний
    ehci_write(ctrl, ctrl->cap_regs->cap_length + EHCI_USBSTS_OFFSET, usbsts);
    
    // Отправляем EOI
    pic_send_eoi(ctrl->irq_line);
}

// Инициализация драйвера
bool ehci_init(term_t* term) {
    term_printf(term, "\n=== USB 2.0 (EHCI) Driver Initialization ===\n");
    
    // 1. Обнаруживаем контроллер
    if (!ehci_detect_controller(term)) {
        term_printf(term, "[EHCI] No EHCI controller found\n");
        return false;
    }
    
    // 2. Сбрасываем контроллер
    if (!ehci_reset_controller(&main_controller)) {
        term_printf(term, "[EHCI] Controller reset failed\n");
        return false;
    }
    
    // 3. Запускаем контроллер
    if (!ehci_start_controller(&main_controller)) {
        term_printf(term, "[EHCI] Controller start failed\n");
        return false;
    }
    
    // 4. Регистрируем обработчик прерываний в IDT
    term_printf(term, "[EHCI] Registering IRQ %d handler in IDT...\n", 
                main_controller.irq_line);
    
    // Вычисляем номер прерывания (IRQ + 32)
    uint8_t interrupt_num = main_controller.irq_line + 32;
    
    // Регистрируем обработчик в IDT
    idt_set_gate(interrupt_num, ehci_irq_handler, KERNEL_CODE_SEL, IDT_GATE_INT);
    
    // Включаем прерывание в PIC
    if (main_controller.irq_line < 16) {
        term_printf(term, "[EHCI] IRQ %d enabled in PIC (INT %d)\n", 
                   main_controller.irq_line, interrupt_num);
    }
    
    // 5. Сканируем подключенные устройства
    term_printf(term, "[EHCI] Scanning connected devices...\n");
    for (uint8_t port = 0; port < main_controller.num_ports; port++) {
        uint32_t portsc = ehci_read(&main_controller, 
                                   main_controller.cap_regs->cap_length + 
                                   EHCI_PORTSC_OFFSET + (port * 4));
        
        if (portsc & (1 << 1)) { // Connected
            term_printf(term, "[EHCI] Found device on port %d\n", port);
            if (ehci_port_reset(&main_controller, port)) {
                ehci_enumerate_device(&main_controller, port);
            }
        }
    }
    
    term_printf(term, "[EHCI] Initialized successfully. Found %d devices\n",
                main_controller.device_count);
    
    return true;
}