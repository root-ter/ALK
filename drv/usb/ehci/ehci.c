// drv/usb/ehci.c
#include "ehci.h"
#include "../../pci/pci.h"
#include "../../io/io.h"
#include "../../../base/mem/mem.h"
#include "../../../base/mem/pmm.h"
#include "../../../libc/string.h"
#include "../../../base/int/idt.h"
#include "../../pic/pic.h"

// ==================== EHCI REGISTERS ====================

// Capability registers
#define EHCI_CAPLENGTH      0x00
#define EHCI_HCIVERSION     0x02
#define EHCI_HCSPARAMS      0x04
#define EHCI_HCCPARAMS      0x08

// Operational registers
#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_USBINTR        0x08
#define EHCI_FRINDEX        0x0C
#define EHCI_CTRLDSSEGMENT  0x10
#define EHCI_PERIODICLIST   0x14
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC         0x44

// USBCMD bits
#define EHCI_CMD_RUN         (1 << 0)
#define EHCI_CMD_HCRESET     (1 << 1)
#define EHCI_CMD_FLSIZE_1024 (0 << 2)
#define EHCI_CMD_FLSIZE_512  (1 << 2)
#define EHCI_CMD_FLSIZE_256  (2 << 2)
#define EHCI_CMD_PSENABLE    (1 << 4)
#define EHCI_CMD_ASENABLE    (1 << 5)
#define EHCI_CMD_IAAD        (1 << 6)

// USBSTS bits
#define EHCI_STS_USBINT      (1 << 0)
#define EHCI_STS_USBERR      (1 << 1)
#define EHCI_STS_PORTCHG     (1 << 2)
#define EHCI_STS_FRAME       (1 << 3)
#define EHCI_STS_HOSTSYSERR  (1 << 4)
#define EHCI_STS_IAAD        (1 << 5)
#define EHCI_STS_HALTED      (1 << 12)
#define EHCI_STS_RECLAM      (1 << 13)
#define EHCI_STS_PERIODIC    (1 << 14)
#define EHCI_STS_ASYNC       (1 << 15)

// PORTSC bits
#define EHCI_PORT_CONN       (1 << 0)
#define EHCI_PORT_CONNCHG    (1 << 1)
#define EHCI_PORT_ENABLE     (1 << 2)
#define EHCI_PORT_ENCHG      (1 << 3)
#define EHCI_PORT_OWND       (1 << 4)
#define EHCI_PORT_OVER       (1 << 5)
#define EHCI_PORT_OVERCHG    (1 << 6)
#define EHCI_PORT_RESET      (1 << 8)
#define EHCI_PORT_POWER      (1 << 12)
#define EHCI_PORT_SPEED_SHIFT 26
#define EHCI_PORT_SPEED_MASK (3 << 26)
#define EHCI_PORT_SPEED_FULL (1 << 26)
#define EHCI_PORT_SPEED_LOW  (2 << 26)
#define EHCI_PORT_SPEED_HIGH (3 << 26)

// Link pointer types
#define EHCI_LINK_TERMINATE  (1 << 0)
#define EHCI_LINK_TYPE_ITD   (0 << 1)
#define EHCI_LINK_TYPE_QH    (1 << 1)
#define EHCI_LINK_TYPE_SITD  (2 << 1)
#define EHCI_LINK_TYPE_FSTN  (3 << 1)

// QTD token bits
#define QTD_PID_OUT     (0 << 8)    // 0x0000
#define QTD_PID_IN      (1 << 8)    // 0x0100
#define QTD_PID_SETUP   (2 << 8)    // 0x0200

// Статусные биты
#define QTD_ACTIVE      (1 << 7)    // 0x0080
#define QTD_HALTED      (1 << 6)    // 0x0040
#define QTD_DATABUFF_ERR (1 << 5)   // 0x0020
#define QTD_BABBLE      (1 << 4)    // 0x0010
#define QTD_XACT_ERR    (1 << 3)    // 0x0008
#define QTD_MISSED      (1 << 2)    // 0x0004
#define QTD_SPLIT       (1 << 1)    // 0x0002
#define QTD_PING        (1 << 0)    // 0x0001

// Длина и IOC
#define QTD_LENGTH(n)   ((n) << 16)  // bits 16-31
#define QTD_IOC         (1 << 15)    // Interrupt on complete (bit 15)
#define QTD_CERR(n)     ((n) << 10)  // Error counter (bits 10-11)

// ==================== STRUCTURES ====================

// Capability registers
typedef struct {
    uint8_t  caplength;
    uint8_t  reserved;
    uint16_t hciversion;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint32_t hcsp_portroute[2];
} __attribute__((packed)) ehci_cap_regs_t;

// Operational registers
typedef struct {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t usbintr;
    uint32_t frindex;
    uint32_t ctrldssegment;
    uint32_t periodiclist;
    uint32_t asynclistaddr;
    uint32_t reserved[9];
    uint32_t configflag;
    uint32_t portsc[15];
} __attribute__((packed)) ehci_op_regs_t;

// Queue Head (MUST be 32-byte aligned!)
typedef struct {
    uint32_t horiz_link;      // Horizontal link pointer
    uint32_t ep_char;         // Endpoint characteristics
    uint32_t next_qtd;        // Next qTD pointer
    uint32_t alt_next_qtd;    // Alternate next qTD
    uint32_t token;           // Token
    uint32_t buffer[5];       // Buffer pointers
    uint32_t reserved[4];     // Reserved
} __attribute__((packed, aligned(32))) ehci_qh_t;

// Queue Transfer Descriptor (MUST be 32-byte aligned!)
typedef struct {
    uint32_t next_qtd;        // Next qTD pointer
    uint32_t alt_next_qtd;    // Alternate next qTD
    uint32_t token;           // Token
    uint32_t buffer[5];       // Buffer pointers
    uint32_t reserved[3];     // Reserved
} __attribute__((packed, aligned(32))) ehci_qtd_t;

// Transfer request
typedef struct {
    usb_device_t* dev;
    uint8_t       endpoint;
    uint8_t       type;        // 0=control, 1=bulk, 2=interrupt
    uint8_t       direction;
    void*         buffer;
    uint32_t      length;
    uint32_t      actual;
    int           status;
    void          (*callback)(void*);
    void*         user_data;
    
    // EHCI specific
    ehci_qh_t*    qh;
    ehci_qtd_t*   qtd;
    uint32_t      qtd_phys;
} ehci_transfer_t;

// EHCI controller
typedef struct {
    // MMIO registers
    ehci_cap_regs_t* cap_regs;
    ehci_op_regs_t*  op_regs;
    uint64_t         mmio_base;
    
    // Parameters
    uint8_t  num_ports;
    uint8_t  num_cc;
    uint8_t  irq;
    bool     has_64bit;
    
    // DMA structures
    uint32_t*    frame_list;
    uint32_t     frame_list_phys;
    ehci_qh_t*   async_qh;
    uint32_t     async_qh_phys;
    ehci_qh_t*   periodic_qh;
    uint32_t     periodic_qh_phys;
    
    // USB core interface
    usb_controller_ops_t ops;
    int                  controller_id;
    
    // PMM
    pmm_t*       pmm;
    
    // State
    bool         initialized;
} ehci_controller_t;

static ehci_controller_t g_ehci;
extern term_t* term;

// ==================== LOW-LEVEL FUNCTIONS ====================

static inline uint32_t ehci_read_op(ehci_controller_t* ctrl, uint32_t reg) {
    volatile uint32_t* ptr = (volatile uint32_t*)((uintptr_t)ctrl->op_regs + reg);
    return *ptr;
}

static inline void ehci_write_op(ehci_controller_t* ctrl, uint32_t reg, uint32_t val) {
    volatile uint32_t* ptr = (volatile uint32_t*)((uintptr_t)ctrl->op_regs + reg);
    *ptr = val;
}

static inline uint32_t ehci_read_cap(ehci_controller_t* ctrl, uint32_t reg) {
    volatile uint32_t* ptr = (volatile uint32_t*)((uintptr_t)ctrl->cap_regs + reg);
    return *ptr;
}

// ==================== DMA ALLOCATION ====================

static void* ehci_alloc_dma(ehci_controller_t* ctrl) {
    if (!ctrl->pmm) return NULL;
    
    void* phys = pmm_alloc_page(ctrl->pmm);
    if (!phys) return NULL;
    
    uint64_t addr = (uint64_t)phys;
    if (addr > 0xFFFFFFFF) {
        term_printf(term, "[EHCI] ERROR: DMA >4GB\n");
        pmm_free_page(ctrl->pmm, phys);
        return NULL;
    }
    
    // Check 32-byte alignment for QH/QTD
    if (addr & 0x1F) {
        // Allocate second page for alignment
        void* phys2 = pmm_alloc_page(ctrl->pmm);
        if (!phys2) {
            pmm_free_page(ctrl->pmm, phys);
            return NULL;
        }
        
        uint64_t addr2 = (uint64_t)phys2;
        uint64_t aligned = (addr2 + 31) & ~31;
        
        // Check if aligned address fits in page
        if (aligned + 32 > addr2 + PAGE_SIZE) {
            pmm_free_page(ctrl->pmm, phys);
            pmm_free_page(ctrl->pmm, phys2);
            return NULL;
        }
        
        pmm_free_page(ctrl->pmm, phys);
        memset((void*)(uintptr_t)addr2, 0, PAGE_SIZE);
        return (void*)(uintptr_t)aligned;
    }
    
    memset(phys, 0, PAGE_SIZE);
    return phys;
}

static void ehci_free_dma(ehci_controller_t* ctrl, void* ptr) {
    if (ctrl->pmm && ptr) {
        void* page = (void*)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
        pmm_free_page(ctrl->pmm, page);
    }
}

// ==================== CONTROLLER INIT ====================

static bool ehci_find_controller(ehci_controller_t* ctrl) {
    term_printf(term, "[EHCI] Scanning for controller...\n");
    
    // Find USB 2.0 controller (class=0x0C, subclass=0x03)
    pci_device_t* pci = pci_find_class(0x0C, 0x03);
    if (!pci) {
        term_printf(term, "[EHCI] No controller found\n");
        return false;
    }
    
    // Check if it's EHCI (ProgIF=0x20)
    if (pci->prog_if != 0x20) {
        term_printf(term, "[EHCI] Not EHCI (ProgIF=0x%02X)\n", pci->prog_if);
        free(pci);
        return false;
    }
    
    term_printf(term, "[EHCI] Found at %02X:%02X.%X\n", 
                pci->bus, pci->slot, pci->function);
    term_printf(term, "[EHCI] VID=0x%04X, DID=0x%04X\n", 
                pci->vendor_id, pci->device_id);
    
    // Enable device
    pci_enable(pci);
    pci_enable_busmaster(pci);
    
    // Get BAR0
    uint64_t bar = pci->bars[0] & ~0xF;
    if (!bar) {
        term_printf(term, "[EHCI] No BAR0\n");
        free(pci);
        return false;
    }
    
    ctrl->mmio_base = bar;
    ctrl->cap_regs = (ehci_cap_regs_t*)(uintptr_t)bar;
    ctrl->op_regs = (ehci_op_regs_t*)(uintptr_t)(bar + ctrl->cap_regs->caplength);
    
    // Get parameters
    uint32_t hcsp = ctrl->cap_regs->hcsparams;
    ctrl->num_ports = hcsp & 0xF;
    ctrl->num_cc = (hcsp >> 12) & 0xF;
    
    uint32_t hccp = ctrl->cap_regs->hccparams;
    ctrl->has_64bit = (hccp & 1) ? true : false;
    
    ctrl->irq = pci->interrupt_line;
    
    term_printf(term, "[EHCI] MMIO: 0x%llx, Ports: %d, IRQ: %d\n", 
                bar, ctrl->num_ports, ctrl->irq);
    term_printf(term, "[EHCI] 64-bit: %s\n", ctrl->has_64bit ? "Yes" : "No");
    
    free(pci);
    return true;
}

static bool ehci_reset(ehci_controller_t* ctrl) {
    term_printf(term, "[EHCI] Resetting...\n");
    
    // Stop controller
    ehci_write_op(ctrl, EHCI_USBCMD, 0);
    
    // Wait for halt
    for (int i = 0; i < 1000; i++) {
        if (ehci_read_op(ctrl, EHCI_USBSTS) & EHCI_STS_HALTED)
            break;
        for (volatile int j = 0; j < 1000; j++);
    }
    
    // Hardware reset
    ehci_write_op(ctrl, EHCI_USBCMD, EHCI_CMD_HCRESET);
    
    // Wait for reset complete
    for (int i = 0; i < 1000; i++) {
        if (!(ehci_read_op(ctrl, EHCI_USBCMD) & EHCI_CMD_HCRESET))
            break;
        for (volatile int j = 0; j < 1000; j++);
    }
    
    // Clear status
    ehci_write_op(ctrl, EHCI_USBSTS, 0x3F);
    
    term_printf(term, "[EHCI] Reset complete\n");
    return true;
}

static bool ehci_start(ehci_controller_t* ctrl) {
    term_printf(term, "[EHCI] Starting...\n");
    
    // 1. Allocate frame list
    ctrl->frame_list = (uint32_t*)ehci_alloc_dma(ctrl);
    if (!ctrl->frame_list) {
        term_printf(term, "[EHCI] Failed to allocate frame list\n");
        return false;
    }
    
    ctrl->frame_list_phys = (uint32_t)(uintptr_t)ctrl->frame_list;
    term_printf(term, "[EHCI] Frame list at 0x%08X\n", ctrl->frame_list_phys);
    
    // Initialize frame list with terminators
    for (int i = 0; i < 1024; i++) {
        ctrl->frame_list[i] = EHCI_LINK_TERMINATE;
    }
    
    // 2. Allocate async QH
    ctrl->async_qh = (ehci_qh_t*)ehci_alloc_dma(ctrl);
    if (!ctrl->async_qh) {
        ehci_free_dma(ctrl, ctrl->frame_list);
        return false;
    }
    
    ctrl->async_qh_phys = (uint32_t)(uintptr_t)ctrl->async_qh;
    term_printf(term, "[EHCI] Async QH at 0x%08X\n", ctrl->async_qh_phys);
    
    // Initialize async QH
    memset(ctrl->async_qh, 0, sizeof(ehci_qh_t));
    
    // Horizontal link points to itself (circular list)
    ctrl->async_qh->horiz_link = ctrl->async_qh_phys | EHCI_LINK_TYPE_QH;
    
    // Endpoint characteristics
    // [31:16] = max packet (64)
    // [15]    = head of list (1)
    // [14:12] = EPS (2 = high speed)
    // [11:8]  = endpoint (0)
    // [7]     = I (0)
    // [6:0]   = type (0 = control)
    ctrl->async_qh->ep_char = 
        (64 << 16) |    // Max packet
        (1 << 15) |     // Head
        (2 << 12) |     // High speed
        (0 << 8);       // Endpoint 0
    
    // Overlay area - all terminated
    ctrl->async_qh->next_qtd = EHCI_LINK_TERMINATE;
    ctrl->async_qh->alt_next_qtd = EHCI_LINK_TERMINATE;
    ctrl->async_qh->token = QTD_HALTED;
    
    // 3. Configure registers
    if (ctrl->has_64bit) {
        ehci_write_op(ctrl, EHCI_CTRLDSSEGMENT, 0);
    }
    
    // Set periodic list
    ehci_write_op(ctrl, EHCI_PERIODICLIST, ctrl->frame_list_phys);
    
    // Set async list
    ehci_write_op(ctrl, EHCI_ASYNCLISTADDR, ctrl->async_qh_phys);
    
    uint32_t async_check = ehci_read_op(ctrl, EHCI_ASYNCLISTADDR);
    term_printf(term, "[EHCI] ASYNCLISTADDR=0x%08X\n", async_check);
    
    // 4. Start controller
    uint32_t cmd = EHCI_CMD_RUN | 
                   EHCI_CMD_ASENABLE | 
                   EHCI_CMD_PSENABLE |
                   EHCI_CMD_FLSIZE_1024;
    
    ehci_write_op(ctrl, EHCI_USBCMD, cmd);
    term_printf(term, "[EHCI] USB_CMD=0x%08X\n", cmd);
    
    // 5. Wait for start
    for (int i = 0; i < 100; i++) {
        uint32_t sts = ehci_read_op(ctrl, EHCI_USBSTS);
        
        if (!(sts & EHCI_STS_HALTED)) {
            term_printf(term, "[EHCI] Started at iteration %d\n", i);
            
            if (sts & EHCI_STS_ASYNC)
                term_printf(term, "[EHCI] Async list active\n");
            if (sts & EHCI_STS_PERIODIC)
                term_printf(term, "[EHCI] Periodic list active\n");
            
            ctrl->initialized = true;
            return true;
        }
        
        for (volatile int j = 0; j < 100000; j++);
    }
    
    uint32_t sts = ehci_read_op(ctrl, EHCI_USBSTS);
    term_printf(term, "[EHCI] Failed to start, STS=0x%08X\n", sts);
    return false;
}

// ==================== PORT MANAGEMENT ====================

static uint8_t ehci_speed_to_usb(uint32_t portsc) {
    uint32_t speed = (portsc >> EHCI_PORT_SPEED_SHIFT) & 3;
    switch(speed) {
        case 0: return USB_SPEED_FULL;  // Full speed
        case 1: return USB_SPEED_LOW;   // Low speed
        case 2: return USB_SPEED_HIGH;  // High speed
        default: return USB_SPEED_FULL;
    }
}

static void ehci_port_reset(ehci_controller_t* ctrl, int port) {
    term_printf(term, "[EHCI] Resetting port %d...\n", port);
    
    uint32_t portsc = ehci_read_op(ctrl, EHCI_PORTSC + port * 4);
    
    // Start reset
    portsc |= EHCI_PORT_RESET;
    ehci_write_op(ctrl, EHCI_PORTSC + port * 4, portsc);
    
    // Wait 50ms
    for (volatile int i = 0; i < 5000000; i++);
    
    // End reset
    portsc &= ~EHCI_PORT_RESET;
    ehci_write_op(ctrl, EHCI_PORTSC + port * 4, portsc);
    
    // Wait for enable
    for (int i = 0; i < 1000; i++) {
        portsc = ehci_read_op(ctrl, EHCI_PORTSC + port * 4);
        if (portsc & EHCI_PORT_ENABLE) break;
        for (volatile int j = 0; j < 10000; j++);
    }
}

static void ehci_handle_port_change(ehci_controller_t* ctrl, int port) {
    uint32_t portsc = ehci_read_op(ctrl, EHCI_PORTSC + port * 4);
    
    // Clear change bits
    ehci_write_op(ctrl, EHCI_PORTSC + port * 4, 
                  portsc | EHCI_PORT_CONNCHG | EHCI_PORT_ENCHG | EHCI_PORT_OVERCHG);
    
    if (portsc & EHCI_PORT_CONN) {
        // Device connected
        uint8_t speed = ehci_speed_to_usb(portsc);
        term_printf(term, "[EHCI] Device connected on port %d (%s speed)\n", 
                    port, speed == USB_SPEED_HIGH ? "High" : 
                           speed == USB_SPEED_FULL ? "Full" : "Low");
        
        // Reset port
        ehci_port_reset(ctrl, port);
        
        // Create USB device
        usb_device_t* dev = usb_device_add(ctrl, port, speed);
        if (dev) {
            dev->controller_data = ctrl;
            usb_device_enumerate(dev);
        }
    } else {
        // Device disconnected
        term_printf(term, "[EHCI] Device disconnected on port %d\n", port);
        
        // Find and remove device
        // TODO: Implement device removal
    }
}

static void ehci_scan_ports(ehci_controller_t* ctrl) {
    term_printf(term, "[EHCI] Scanning ports...\n");
    
    for (int port = 0; port < ctrl->num_ports; port++) {
        uint32_t portsc = ehci_read_op(ctrl, EHCI_PORTSC + port * 4);
        
        if (portsc & EHCI_PORT_CONN) {
            ehci_handle_port_change(ctrl, port);
        }
    }
}

// ==================== IRQ HANDLER ====================

void ehci_irq_handler(void) {
    ehci_controller_t* ctrl = &g_ehci;
    
    if (!ctrl->initialized) {
        if (ctrl->irq < 16) pic_send_eoi(ctrl->irq);
        return;
    }
    
    uint32_t sts = ehci_read_op(ctrl, EHCI_USBSTS);
    
    // Handle port changes
    if (sts & EHCI_STS_PORTCHG) {
        for (int port = 0; port < ctrl->num_ports; port++) {
            uint32_t portsc = ehci_read_op(ctrl, EHCI_PORTSC + port * 4);
            
            if (portsc & (EHCI_PORT_CONNCHG | EHCI_PORT_ENCHG | EHCI_PORT_OVERCHG)) {
                ehci_handle_port_change(ctrl, port);
            }
        }
    }
    
    // Handle USB interrupts
    if (sts & EHCI_STS_USBINT) {
        // Transfer complete
    }
    
    // Handle errors
    if (sts & EHCI_STS_USBERR) {
        term_printf(term, "[EHCI] USB error\n");
    }
    
    // Clear status
    ehci_write_op(ctrl, EHCI_USBSTS, sts);
    
    if (ctrl->irq < 16) pic_send_eoi(ctrl->irq);
}

// ==================== USB CORE CALLBACKS ====================

// ehci_control_transfer - АСИНХРОННЫЙ контрольный трансфер
static int ehci_control_transfer(usb_device_t* dev, uint8_t bmRequestType,
                                 uint8_t bRequest, uint16_t wValue,
                                 uint16_t wIndex, uint16_t wLength,
                                 void* data, int timeout_ms) {
    ehci_controller_t* ctrl = (ehci_controller_t*)dev->controller_data;
    if (!ctrl || !ctrl->initialized) return -1;
    
    term_printf(term, "[EHCI] Control: req=0x%02X, len=%d\n", bRequest, wLength);
    
    // 1. Выделяем qTD для SETUP стадии
    ehci_qtd_t* setup_qtd = ehci_alloc_dma(ctrl);
    if (!setup_qtd) return -1;
    memset(setup_qtd, 0, sizeof(ehci_qtd_t));
    
    // Формируем SETUP пакет (8 байт)
    uint8_t* setup_buf = (uint8_t*)ehci_alloc_dma(ctrl);
    if (!setup_buf) {
        ehci_free_dma(ctrl, setup_qtd);
        return -1;
    }
    
    setup_buf[0] = bmRequestType;
    setup_buf[1] = bRequest;
    setup_buf[2] = wValue & 0xFF;
    setup_buf[3] = (wValue >> 8) & 0xFF;
    setup_buf[4] = wIndex & 0xFF;
    setup_buf[5] = (wIndex >> 8) & 0xFF;
    setup_buf[6] = wLength & 0xFF;
    setup_buf[7] = (wLength >> 8) & 0xFF;
    
    // Настраиваем SETUP qTD
    setup_qtd->next_qtd = EHCI_LINK_TERMINATE;
    setup_qtd->alt_next_qtd = EHCI_LINK_TERMINATE;
    setup_qtd->token = QTD_ACTIVE | QTD_PID_SETUP | QTD_LENGTH(8);
    setup_qtd->buffer[0] = (uint32_t)(uintptr_t)setup_buf;
    
    uint32_t setup_phys = (uint32_t)(uintptr_t)setup_qtd;
    
    // 2. DATA стадия (если есть данные)
    ehci_qtd_t* data_qtd = NULL;
    uint32_t data_phys = 0;
    void* data_buf = NULL;
    
    if (wLength > 0 && data) {
        data_qtd = ehci_alloc_dma(ctrl);
        if (!data_qtd) {
            ehci_free_dma(ctrl, setup_qtd);
            ehci_free_dma(ctrl, setup_buf);
            return -1;
        }
        memset(data_qtd, 0, sizeof(ehci_qtd_t));
        
        // Копируем данные в DMA-буфер
        data_buf = ehci_alloc_dma(ctrl);
        if (!data_buf) {
            ehci_free_dma(ctrl, setup_qtd);
            ehci_free_dma(ctrl, setup_buf);
            ehci_free_dma(ctrl, data_qtd);
            return -1;
        }
        
        memcpy(data_buf, data, wLength);
        
        uint32_t pid;
		if (bmRequestType & 0x80) {
    		pid = QTD_PID_IN;
		} else {
		    pid = QTD_PID_OUT;
		}
        
        data_qtd->next_qtd = EHCI_LINK_TERMINATE;
        data_qtd->alt_next_qtd = EHCI_LINK_TERMINATE;
        data_qtd->token = QTD_ACTIVE | pid | QTD_LENGTH(wLength);
        data_qtd->buffer[0] = (uint32_t)(uintptr_t)data_buf;
        
        data_phys = (uint32_t)(uintptr_t)data_qtd;
        
        // Связываем: SETUP -> DATA
        setup_qtd->next_qtd = data_phys;
    }
    
    // 3. STATUS стадия
    ehci_qtd_t* status_qtd = ehci_alloc_dma(ctrl);
    if (!status_qtd) {
        ehci_free_dma(ctrl, setup_qtd);
        ehci_free_dma(ctrl, setup_buf);
        if (data_qtd) ehci_free_dma(ctrl, data_qtd);
        if (data_buf) ehci_free_dma(ctrl, data_buf);
        return -1;
    }
    memset(status_qtd, 0, sizeof(ehci_qtd_t));
    
    uint8_t status_pid = (bmRequestType & 0x80) ? QTD_PID_OUT : QTD_PID_IN;
    status_qtd->next_qtd = EHCI_LINK_TERMINATE;
    status_qtd->alt_next_qtd = EHCI_LINK_TERMINATE;
    status_qtd->token = QTD_ACTIVE | status_pid | QTD_LENGTH(0) | QTD_IOC;
    
    uint32_t status_phys = (uint32_t)(uintptr_t)status_qtd;
    
    // Связываем последний qTD со STATUS
    if (data_qtd) {
        data_qtd->next_qtd = status_phys;
    } else {
        setup_qtd->next_qtd = status_phys;
    }
    
    // 4. Подвешиваем к ASYNC списку
    // Сохраняем старый указатель
    uint32_t old_next = ctrl->async_qh->horiz_link;
    
    // Создаём временный QH для этого трансфера
    ehci_qh_t* temp_qh = ehci_alloc_dma(ctrl);
    if (!temp_qh) {
        // clean up
        return -1;
    }
    memset(temp_qh, 0, sizeof(ehci_qh_t));
    
    temp_qh->horiz_link = old_next;  // Включаем в цепочку
    temp_qh->ep_char = (dev->device_desc.bMaxPacketSize0 << 16) | (dev->address << 8) | (0 << 15) | (2 << 12);
    temp_qh->next_qtd = setup_phys;
    temp_qh->alt_next_qtd = EHCI_LINK_TERMINATE;
    temp_qh->token = QTD_HALTED;
    
    // Вставляем в начало async списка
    ctrl->async_qh->horiz_link = (uint32_t)(uintptr_t)temp_qh | EHCI_LINK_TYPE_QH;
    
    // Активируем async schedule если надо
    uint32_t cmd = ehci_read_op(ctrl, EHCI_USBCMD);
    if (!(cmd & EHCI_CMD_ASENABLE)) {
        ehci_write_op(ctrl, EHCI_USBCMD, cmd | EHCI_CMD_ASENABLE);
    }
    
    // 5. Ждём завершения (polling режим)
    uint32_t timeout = timeout_ms * 1000;
    while (timeout--) {
        uint32_t token = status_qtd->token;
        if (!(token & QTD_ACTIVE)) {
            // Завершилось
            if (token & QTD_HALTED) {
                // Ошибка
                term_printf(term, "[EHCI] Control transfer failed: token=0x%08X\n", token);
                
                // Восстанавливаем
                ctrl->async_qh->horiz_link = old_next;
                
                // Clean up
                ehci_free_dma(ctrl, temp_qh);
                ehci_free_dma(ctrl, setup_qtd);
                ehci_free_dma(ctrl, setup_buf);
                if (data_qtd) ehci_free_dma(ctrl, data_qtd);
                if (data_buf) ehci_free_dma(ctrl, data_buf);
                ehci_free_dma(ctrl, status_qtd);
                
                return -1;
            }
            
            // Успех - копируем данные обратно если нужно
            if (data && wLength > 0 && (bmRequestType & 0x80)) {
                memcpy(data, data_buf, wLength);
            }
            
            // Восстанавливаем
            ctrl->async_qh->horiz_link = old_next;
            
            // Clean up
            ehci_free_dma(ctrl, temp_qh);
            ehci_free_dma(ctrl, setup_qtd);
            ehci_free_dma(ctrl, setup_buf);
            if (data_qtd) ehci_free_dma(ctrl, data_qtd);
            if (data_buf) ehci_free_dma(ctrl, data_buf);
            ehci_free_dma(ctrl, status_qtd);
            
            return wLength;
        }
        
        // Небольшая задержка
        for (volatile int i = 0; i < 10; i++);
    }
    
    // Timeout
    term_printf(term, "[EHCI] Control transfer timeout\n");
    
    // Восстанавливаем
    ctrl->async_qh->horiz_link = old_next;
    
    // Clean up
    ehci_free_dma(ctrl, temp_qh);
    ehci_free_dma(ctrl, setup_qtd);
    ehci_free_dma(ctrl, setup_buf);
    if (data_qtd) ehci_free_dma(ctrl, data_qtd);
    if (data_buf) ehci_free_dma(ctrl, data_buf);
    ehci_free_dma(ctrl, status_qtd);
    
    return -1;
}

static int ehci_bulk_transfer(usb_device_t* dev, uint8_t endpoint,
                              void* data, int length, int timeout_ms) {
    ehci_controller_t* ctrl = (ehci_controller_t*)dev->controller_data;
    if (!ctrl || !ctrl->initialized) return -1;
    
    // Определяем направление
    uint32_t dir;
	if (endpoint & 0x80) {
	    dir = QTD_PID_IN;
	} else {
	    dir = QTD_PID_OUT;
	}
    uint8_t ep_num = endpoint & 0x7F;
    
    // Выделяем qTD
    ehci_qtd_t* qtd = ehci_alloc_dma(ctrl);
    if (!qtd) return -1;
    memset(qtd, 0, sizeof(ehci_qtd_t));
    
    // Выделяем DMA-буфер
    void* dma_buf = ehci_alloc_dma(ctrl);
    if (!dma_buf) {
        ehci_free_dma(ctrl, qtd);
        return -1;
    }
    
    // Копируем данные
    if (dir == QTD_PID_OUT) {
        memcpy(dma_buf, data, length);
    }
    
    // Настраиваем qTD
    qtd->next_qtd = EHCI_LINK_TERMINATE;
    qtd->alt_next_qtd = EHCI_LINK_TERMINATE;
    qtd->token = QTD_ACTIVE | dir | QTD_LENGTH(length) | QTD_IOC;
    qtd->buffer[0] = (uint32_t)(uintptr_t)dma_buf;
    
    uint32_t qtd_phys = (uint32_t)(uintptr_t)qtd;
    
    // Создаём QH для этого эндпоинта
    ehci_qh_t* qh = ehci_alloc_dma(ctrl);
    if (!qh) {
        ehci_free_dma(ctrl, qtd);
        ehci_free_dma(ctrl, dma_buf);
        return -1;
    }
    memset(qh, 0, sizeof(ehci_qh_t));
    
    // Настраиваем QH (max packet обычно 512 для bulk high-speed)
    uint32_t max_packet = 512;
    qh->horiz_link = ctrl->async_qh->horiz_link;
    qh->ep_char = (max_packet << 16) | 
                  (dev->address << 8) | 
                  (ep_num << 8) |
                  (0 << 15) |           // Head of list
                  (2 << 12);             // High speed
    qh->next_qtd = qtd_phys;
    qh->alt_next_qtd = EHCI_LINK_TERMINATE;
    qh->token = QTD_HALTED;
    
    // Вставляем в async список
    uint32_t old_next = ctrl->async_qh->horiz_link;
    ctrl->async_qh->horiz_link = (uint32_t)(uintptr_t)qh | EHCI_LINK_TYPE_QH;
    
    // Ждём завершения
    uint32_t timeout = timeout_ms * 1000;
    while (timeout--) {
        uint32_t token = qtd->token;
        if (!(token & QTD_ACTIVE)) {
            if (token & QTD_HALTED) {
                term_printf(term, "[EHCI] Bulk transfer failed\n");
                ctrl->async_qh->horiz_link = old_next;
                ehci_free_dma(ctrl, qh);
                ehci_free_dma(ctrl, qtd);
                if (dir == QTD_PID_IN) {
                    memcpy(data, dma_buf, length);
                }
                ehci_free_dma(ctrl, dma_buf);
                return -1;
            }
            
            // Успех
            ctrl->async_qh->horiz_link = old_next;
            if (dir == QTD_PID_IN) {
                memcpy(data, dma_buf, length);
            }
            ehci_free_dma(ctrl, qh);
            ehci_free_dma(ctrl, qtd);
            ehci_free_dma(ctrl, dma_buf);
            return length;
        }
        for (volatile int i = 0; i < 10; i++);
    }
    
    // Timeout
    ctrl->async_qh->horiz_link = old_next;
    ehci_free_dma(ctrl, qh);
    ehci_free_dma(ctrl, qtd);
    ehci_free_dma(ctrl, dma_buf);
    return -1;
}

static int ehci_interrupt_transfer(usb_device_t* dev, uint8_t endpoint,
                                   void* data, int length,
                                   void (*callback)(usb_transfer_t*)) {
    ehci_controller_t* ctrl = (ehci_controller_t*)dev->controller_data;
    if (!ctrl || !ctrl->initialized) return -1;
    
    term_printf(term, "[EHCI] Interrupt transfer EP 0x%02X\n", endpoint);
    return 0;
}

static int ehci_reset_device(usb_device_t* dev) {
    ehci_controller_t* ctrl = (ehci_controller_t*)dev->controller_data;
    if (!ctrl || !ctrl->initialized) return -1;
    
    term_printf(term, "[EHCI] Reset device %d\n", dev->address);
    return 0;
}

static void ehci_print_info(void) {
    term_printf(term, "[EHCI] Controller at 0x%llx, %d ports\n",
                g_ehci.mmio_base, g_ehci.num_ports);
}

// Controller operations table
static usb_controller_ops_t ehci_ops = {
    .name = "EHCI",
    .control_transfer = ehci_control_transfer,
    .bulk_transfer = ehci_bulk_transfer,
    .interrupt_transfer = ehci_interrupt_transfer,
    .reset_device = ehci_reset_device,
    .print_info = ehci_print_info
};

// ==================== INITIALIZATION ====================

bool ehci_init(term_t* term, pmm_t* pmm) {
    term_printf(term, "\n=== EHCI Driver ===\n");
    
    memset(&g_ehci, 0, sizeof(g_ehci));
    g_ehci.pmm = pmm;
    
    // 1. Find controller
    if (!ehci_find_controller(&g_ehci)) {
        return false;
    }
    
    // 2. Reset
    if (!ehci_reset(&g_ehci)) {
        return false;
    }
    
    // 3. Start
    if (!ehci_start(&g_ehci)) {
        return false;
    }
    
    // 4. Setup IRQ
    if (g_ehci.irq < 16) {
        uint8_t vec = g_ehci.irq + 32;
        idt_set_gate(vec, ehci_irq_handler, 0x08, 0x8E);
        
        // Unmask in PIC
        if (g_ehci.irq < 8) {
            uint8_t mask = inb(0x21);
            mask &= ~(1 << g_ehci.irq);
            outb(0x21, mask);
        } else {
            uint8_t mask = inb(0xA1);
            mask &= ~(1 << (g_ehci.irq - 8));
            outb(0xA1, mask);
        }
        
        term_printf(term, "[EHCI] IRQ %d enabled\n", g_ehci.irq);
    }
    
    // 5. Register with USB core
    g_ehci.ops = ehci_ops;
    g_ehci.controller_id = usb_register_controller(&g_ehci.ops, &g_ehci);
    
    // 6. Scan ports
    ehci_scan_ports(&g_ehci);
    
    term_printf(term, "[EHCI] Initialized successfully\n\n");
    return true;
}
