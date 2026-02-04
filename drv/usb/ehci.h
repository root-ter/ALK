// drv/usb/ehci.h
#ifndef EHCI_H
#define EHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/term/term.h"

#define EHCI_CAPLENGTH_OFFSET 0x00
#define EHCI_HCCPARAMS_OFFSET 0x08
#define EHCI_HCSPARAMS_OFFSET 0x04
#define EHCI_USBCMD_OFFSET    0x00
#define EHCI_USBSTS_OFFSET    0x04
#define EHCI_USBINTR_OFFSET   0x08
#define EHCI_FRINDEX_OFFSET   0x0C
#define EHCI_CTRLDSSEG_OFFSET 0x10
#define EHCI_PERIODICLIST_OFFSET 0x14
#define EHCI_ASYNCLIST_OFFSET 0x18
#define EHCI_CONFIGFLAG_OFFSET 0x40
#define EHCI_PORTSC_OFFSET    0x44

// Структура Capability Registers
typedef struct {
    uint8_t cap_length;
    uint8_t reserved;
    uint16_t hci_version;
    uint32_t hcs_params;
    uint32_t hcc_params;
    uint32_t hcsp_portroute[2];
} ehci_cap_regs_t;

// Структура Operational Registers
typedef struct {
    uint32_t usb_cmd;
    uint32_t usb_sts;
    uint32_t usb_intr;
    uint32_t frindex;
    uint32_t ctrlds_segment;
    uint32_t periodic_list;
    uint32_t async_list;
    uint32_t reserved[9];
    uint32_t config_flag;
    uint32_t port_sc[64];
} ehci_op_regs_t;

// QH (Queue Head)
typedef struct __attribute__((packed)) {
    uint32_t horiz_link_ptr;
    uint32_t char_bytes;
    struct {
        uint32_t next_qtd;
        uint32_t alt_next_qtd;
        uint32_t token;
        uint32_t buffer_ptr[5];
    } qtd_overlay;
    uint32_t reserved[4];
} ehci_qh_t;

// QTD (Queue Transfer Descriptor)
typedef struct __attribute__((packed)) {
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer_ptr[5];
    uint32_t extended_buffer_ptr[5];
} ehci_qtd_t;

// ITD (Isochronous Transfer Descriptor)
typedef struct __attribute__((packed)) {
    uint32_t next_link;
    uint32_t transaction[8];
    uint32_t buffer_ptr[7];
    uint32_t extended_buffer_ptr[7];
} ehci_itd_t;

// Структура USB устройства
typedef struct {
    uint8_t address;
    uint8_t port;
    uint8_t speed; // 0 = low, 1 = full, 2 = high
    uint8_t max_packet_size;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t configuration_value;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t endpoint_queue[16];
    bool connected;
} usb_device_t;

// Структура контроллера EHCI
typedef struct {
    ehci_cap_regs_t* cap_regs;
    ehci_op_regs_t* op_regs;
    uintptr_t mmio_base;
    uint8_t num_ports;
    uint8_t num_companion;
    uint16_t frame_list_size;
    uint32_t* frame_list;
    ehci_qh_t* async_qh;
    ehci_qh_t* periodic_qh;
    usb_device_t devices[127]; // Max 127 devices
    int device_count;
    bool initialized;
    bool owned;
    uint8_t irq_line;
} ehci_controller_t;

// Функции
bool ehci_init(term_t* term);
bool ehci_detect_controller(term_t* term);
bool ehci_reset_controller(ehci_controller_t* ctrl);
bool ehci_start_controller(ehci_controller_t* ctrl);
void ehci_stop_controller(ehci_controller_t* ctrl);
bool ehci_port_reset(ehci_controller_t* ctrl, uint8_t port);
usb_device_t* ehci_enumerate_device(ehci_controller_t* ctrl, uint8_t port);
bool ehci_control_transfer(usb_device_t* dev, uint8_t request_type,
                           uint8_t request, uint16_t value, 
                           uint16_t index, uint16_t length,
                           void* data);
void ehci_irq_handler(void);

#endif