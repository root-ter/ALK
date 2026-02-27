// ahci.h
#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

// ==================== FIS TYPES ====================
#define FIS_TYPE_REG_H2D    0x27    // Register FIS - host to device
#define FIS_TYPE_REG_D2H    0x34    // Register FIS - device to host
#define FIS_TYPE_DMA_ACT    0x39    // DMA activate FIS - device to host
#define FIS_TYPE_DMA_SETUP  0x41    // DMA setup FIS - bidirectional
#define FIS_TYPE_DATA       0x46    // Data FIS - bidirectional
#define FIS_TYPE_BIST       0x58    // BIST activate FIS - bidirectional
#define FIS_TYPE_PIO_SETUP  0x5F    // PIO setup FIS - device to host
#define FIS_TYPE_DEV_BITS   0xA1    // Set device bits FIS - device to host

// ==================== SATA SIGNATURES ====================
#define SATA_SIG_SATA       0x00000101  // SATA drive
#define SATA_SIG_ATAPI      0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB       0xC33C0101  // Enclosure management bridge
#define SATA_SIG_PM         0x96690101  // Port multiplier

// ==================== AHCI GLOBAL REGISTERS ====================
#define AHCI_GHC_HR         (1 << 0)    // HBA Reset
#define AHCI_GHC_IE         (1 << 1)    // Interrupt enable
#define AHCI_GHC_ENABLE     (1 << 31)   // AHCI Enable

#define AHCI_CAP_S64A       (1 << 31)   // 64-bit addressing
#define AHCI_CAP_NCQ        (1 << 30)   // Support for Native Command Queueing
#define AHCI_CAP_SSS        (1 << 27)   // Supports staggered Spin-up
#define AHCI_CAP_SALP       (1 << 26)   // Supports aggressive link power management
#define AHCI_CAP_FBSS       (1 << 16)   // FIS-based switching supported
#define AHCI_CAP_SSC        (1 << 14)   // Slumber state capable
#define AHCI_CAP_PSC        (1 << 13)   // Partial state capable

#define AHCI_CAP2_NVMHCI    (1 << 1)    // NVMHCI Present
#define AHCI_CAP2_BOHC      (1 << 0)    // BIOS/OS Handoff

#define AHCI_BOHC_BIOS_BUSY     (1 << 4)    // BIOS Busy
#define AHCI_BOHC_OS_OWNERSHIP  (1 << 3)    // OS Ownership Change

// ==================== PORT COMMAND BITS ====================
#define HBA_PXCMD_ST        0x0001      // Start
#define HBA_PXCMD_SUD       0x0002      // Spin-Up Device
#define HBA_PXCMD_POD       0x0004      // Power On Device
#define HBA_PXCMD_FRE       0x0010      // FIS Receive Enable
#define HBA_PXCMD_FR        0x4000      // FIS Receive Running
#define HBA_PXCMD_CR        0x8000      // Command List Running
#define HBA_PXCMD_ASP       0x4000000   // Aggressive Slumber/Partial
#define HBA_PXCMD_ICC       0xF0000000  // Interface Communication Control
#define HBA_PXCMD_ICC_ACTIVE (1 << 28)  // ICC: Active

#define HBA_PXIS_TFES       (1 << 30)   // Task File Error Status

#define HBA_PXSSTS_DET      0x0F        // Device Detection
#define HBA_PXSSTS_DET_INIT     1       // Initialized
#define HBA_PXSSTS_DET_PRESENT  3       // Present

#define HBA_PORT_IPM_ACTIVE 1

// ==================== SCTL ====================
#define SCTL_PORT_DET_INIT      0x1
#define SCTL_PORT_IPM_NOPART    0x100   // No partial state
#define SCTL_PORT_IPM_NOSLUM    0x200   // No slumber state
#define SCTL_PORT_IPM_NODSLP    0x400   // No devslp state

// ==================== STRUCTURES ====================

// Register FIS - Host to Device
typedef struct {
    // DWORD 0
    uint8_t  fis_type;
    uint8_t  pmport : 4;
    uint8_t  rsv0   : 3;
    uint8_t  c      : 1;        // 1: Command, 0: Control
    uint8_t  command;
    uint8_t  featurel;
    
    // DWORD 1
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    
    // DWORD 2
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    
    // DWORD 3
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    
    // DWORD 4
    uint8_t  rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

// Register FIS - Device to Host
typedef struct {
    // DWORD 0
    uint8_t  fis_type;
    uint8_t  pmport : 4;
    uint8_t  rsv0   : 2;
    uint8_t  i      : 1;        // Interrupt bit
    uint8_t  rsv1   : 1;
    uint8_t  status;
    uint8_t  error;
    
    // DWORD 1
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    
    // DWORD 2
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  rsv2;
    
    // DWORD 3
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  rsv3[2];
    
    // DWORD 4
    uint8_t  rsv4[4];
} __attribute__((packed)) fis_reg_d2h_t;

// Data FIS
typedef struct {
    // DWORD 0
    uint8_t  fis_type;
    uint8_t  pmport : 4;
    uint8_t  rsv0   : 4;
    uint8_t  rsv1[2];
    
    // DWORD 1 ~ N
    uint32_t data[1];
} __attribute__((packed)) fis_data_t;

// PIO Setup FIS
typedef struct {
    // DWORD 0
    uint8_t  fis_type;
    uint8_t  pmport : 4;
    uint8_t  rsv0   : 1;
    uint8_t  d      : 1;        // Data transfer direction, 1 - device to host
    uint8_t  i      : 1;        // Interrupt bit
    uint8_t  rsv1   : 1;
    uint8_t  status;
    uint8_t  error;
    
    // DWORD 1
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    
    // DWORD 2
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  rsv2;
    
    // DWORD 3
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  rsv3;
    uint8_t  e_status;          // New value of status register
    
    // DWORD 4
    uint16_t tc;                // Transfer count
    uint8_t  rsv4[2];
} __attribute__((packed)) fis_pio_setup_t;

// DMA Setup FIS
typedef struct {
    // DWORD 0
    uint8_t  fis_type;
    uint8_t  pmport : 4;
    uint8_t  rsv0   : 1;
    uint8_t  d      : 1;        // Data transfer direction, 1 - device to host
    uint8_t  i      : 1;        // Interrupt bit
    uint8_t  a      : 1;        // Auto-activate
    uint8_t  rsved[2];
    
    // DWORD 1 & 2
    uint64_t dma_buffer_id;
    
    // DWORD 3
    uint32_t rsvd;
    
    // DWORD 4
    uint32_t dma_buf_offset;
    
    // DWORD 5
    uint32_t transfer_count;
    
    // DWORD 6
    uint32_t resvd;
} __attribute__((packed)) fis_dma_setup_t;

// HBA Port Register Structure
typedef volatile struct {
    uint32_t clb;           // 0x00, command list base address, 1K-byte aligned
    uint32_t clbu;          // 0x04, command list base address upper 32 bits
    uint32_t fb;            // 0x08, FIS base address, 256-byte aligned
    uint32_t fbu;           // 0x0C, FIS base address upper 32 bits
    uint32_t is;            // 0x10, interrupt status
    uint32_t ie;            // 0x14, interrupt enable
    uint32_t cmd;           // 0x18, command and status
    uint32_t rsv0;          // 0x1C, Reserved
    uint32_t tfd;           // 0x20, task file data
    uint32_t sig;           // 0x24, signature
    uint32_t ssts;          // 0x28, SATA status (SCR0:SStatus)
    uint32_t sctl;          // 0x2C, SATA control (SCR2:SControl)
    uint32_t serr;          // 0x30, SATA error (SCR1:SError)
    uint32_t sact;          // 0x34, SATA active (SCR3:SActive)
    uint32_t ci;            // 0x38, command issue
    uint32_t sntf;          // 0x3C, SATA notification (SCR4:SNotification)
    uint32_t fbs;           // 0x40, FIS-based switch control
    uint32_t rsv1[11];      // 0x44 ~ 0x6F, Reserved
    uint32_t vendor[4];     // 0x70 ~ 0x7F, vendor specific
} __attribute__((packed)) hba_port_t;

// HBA Memory Register Structure
typedef volatile struct {
    // 0x00 - 0x2B, Generic Host Control
    uint32_t cap;           // 0x00, Host capability
    uint32_t ghc;           // 0x04, Global host control
    uint32_t is;            // 0x08, Interrupt status
    uint32_t pi;            // 0x0C, Port implemented
    uint32_t vs;            // 0x10, Version
    uint32_t ccc_ctl;       // 0x14, Command completion coalescing control
    uint32_t ccc_pts;       // 0x18, Command completion coalescing ports
    uint32_t em_loc;        // 0x1C, Enclosure management location
    uint32_t em_ctl;        // 0x20, Enclosure management control
    uint32_t cap2;          // 0x24, Host capabilities extended
    uint32_t bohc;          // 0x28, BIOS/OS handoff control and status
    
    // 0x2C - 0x9F, Reserved
    uint8_t  rsv[0xA0 - 0x2C];
    
    // 0xA0 - 0xFF, Vendor specific registers
    uint8_t  vendor[0x100 - 0xA0];
    
    // 0x100 - 0x10FF, Port control registers
    hba_port_t ports[32];   // 1 ~ 32
} __attribute__((packed)) hba_mem_t;

// HBA FIS Structure
typedef volatile struct {
    fis_dma_setup_t  dsfis;     // DMA Setup FIS
    uint8_t          pad0[4];
    fis_pio_setup_t  psfis;     // PIO Setup FIS
    uint8_t          pad1[12];
    fis_reg_d2h_t    rfis;      // Register – Device to Host FIS
    uint8_t          pad2[4];
    uint8_t          sdbfis[8]; // Set Device Bit FIS
    uint8_t          ufis[64];
    uint8_t          rsv[0x100 - 0xA0];
} __attribute__((packed)) hba_fis_t;

// HBA Command Header
typedef struct {
    // DW0
    uint8_t  cfl   : 5;     // Command FIS length in DWORDS, 2 ~ 16
    uint8_t  a     : 1;     // ATAPI
    uint8_t  w     : 1;     // Write, 1: H2D, 0: D2H
    uint8_t  p     : 1;     // Prefetchable
    uint8_t  r     : 1;     // Reset
    uint8_t  b     : 1;     // BIST
    uint8_t  c     : 1;     // Clear busy upon R_OK
    uint8_t  rsv0  : 1;
    uint8_t  pmp   : 4;     // Port multiplier port
    uint16_t prdtl;         // Physical region descriptor table length in entries
    
    // DW1
    uint32_t prdbc;         // Physical region descriptor byte count transferred
    
    // DW2, 3
    uint32_t ctba;          // Command table descriptor base address
    uint32_t ctbau;         // Command table descriptor base address upper 32 bits
    
    // DW4 - 7
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

// HBA PRDT Entry
typedef struct {
    uint32_t dba;           // Data base address
    uint32_t dbau;          // Data base address upper 32 bits
    uint32_t rsv0;
    
    // DW3
    uint32_t dbc   : 22;    // Byte count, 4M max
    uint32_t rsv1  : 9;
    uint32_t i     : 1;     // Interrupt on completion
} __attribute__((packed)) hba_prdt_entry_t;

// HBA Command Table
typedef struct {
    uint8_t  cfis[64];               // Command FIS
    uint8_t  acmd[16];               // ATAPI command
    uint8_t  rsv[48];                // Reserved
    hba_prdt_entry_t prdt_entry[1];  // Physical region descriptor table entries
} __attribute__((packed)) hba_cmd_tbl_t;

// ==================== AHCI PORT STRUCTURE ====================

typedef enum {
    AHCI_PORT_UNINITIALIZED = 0,
    AHCI_PORT_ERROR = 1,
    AHCI_PORT_ACTIVE = 2,
} ahci_port_status_t;

typedef struct {
    // Registers
    hba_port_t* regs;
    
    // Command structures
    hba_cmd_header_t* cmd_list;     // Virtual address of command list
    hba_fis_t* fis;                 // Virtual address of FIS
    hba_cmd_tbl_t* cmd_tables[8];   // Virtual addresses of command tables
    
    // DMA buffers
    uint64_t phys_buffers[8];       // Physical addresses
    void* virt_buffers[8];           // Virtual addresses
    
    // Synchronization
    volatile int buffer_locks[8];
    volatile int buffer_semaphore;
    volatile int port_lock;
    
    // Status
    ahci_port_status_t status;
    int sector_size;
    uint64_t total_sectors;
    int supports_lba48;
    
    // Port number
    int port_num;
} ahci_port_t;

// ==================== PUBLIC FUNCTIONS ====================

int ahci_init(void);
void ahci_stop_cmd(hba_port_t* port);
void ahci_start_cmd(hba_port_t* port);
int ahci_port_read(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);
int ahci_port_write(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);
ahci_port_t* ahci_get_port(int port_num);

#endif