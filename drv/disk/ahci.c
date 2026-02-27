// ahci.c
#include "ahci.h"
#include "../pci/pci.h"
#include "../../base/term/tio.h"
#include "../../base/mem/pmm.h"
#include "../../base/mem/mem.h"
#include "../../libc/string.h"
#include "../../base/time/timer.h"

// ==================== GLOBALS ====================

static hba_mem_t* g_ahci_hba = NULL;
static uintptr_t g_ahci_phys_base = 0;
static uintptr_t g_ahci_virt_base = 0;
static pci_device_t* g_ahci_pci_dev = NULL;
static ahci_port_t* g_ahci_ports[32];

// Внешние функции из твоего ядра
extern pmm_t pmm;

// ==================== INTERNAL FUNCTIONS ====================

static inline uint64_t ahci_get_ticks(void) {
    // Возвращает текущее время в миллисекундах
    return (uint64_t)seconds * 1000 + tick_time;
}

static inline void ahci_mdelay(uint32_t ms) {
    mwait(ms);
}

static int ahci_find_cmd_slot(hba_port_t* port) {
    // If not set in SACT and CI, the slot is free
    uint32_t slots = port->sact | port->ci;
    for (int i = 0; i < 8; i++) {
        if ((slots & 1) == 0)
            return i;
        slots >>= 1;
    }
    return -1;
}

static void ahci_port_stop_cmd(hba_port_t* port) {
    // Clear ST (bit0)
    port->cmd &= ~HBA_PXCMD_ST;
    
    uint64_t timeout = ahci_get_ticks() + 100; // 100ms timeout
    
    // Wait until FR (bit14), CR (bit15) are cleared
    while ((port->cmd & HBA_PXCMD_CR) && ahci_get_ticks() < timeout) {
        asm volatile("pause");
    }
    
    if (ahci_get_ticks() >= timeout) {
        tio_printf("[AHCI] StopCMD timeout\n");
    }
    
    // Clear FRE (bit4)
    port->cmd &= ~HBA_PXCMD_FRE;
}

static void ahci_port_start_cmd(hba_port_t* port) {
    port->cmd &= ~HBA_PXCMD_ST;
    
    uint64_t timeout = ahci_get_ticks() + 100;
    
    // Wait until CR (bit15) is cleared
    while ((port->cmd & HBA_PXCMD_CR) && ahci_get_ticks() < timeout) {
        asm volatile("pause");
    }
    
    if (ahci_get_ticks() >= timeout) {
        tio_printf("[AHCI] StartCMD timeout\n");
    }
    
    // Set FRE (bit4) and ST (bit0)
    port->cmd |= HBA_PXCMD_FRE;
    port->cmd |= HBA_PXCMD_ST;
}

static void* ahci_alloc_dma_page(void) {
    void* phys = pmm_alloc_page(&pmm);
    if (!phys) return NULL;
    
    // Get virtual address (identity mapping in kernel space)
    return (void*)(uintptr_t)phys;
}

static uintptr_t ahci_phys_addr(void* virt) {
    return (uintptr_t)virt; // Identity mapping
}

static int ahci_port_acquire_buffer(ahci_port_t* port) {
    // Semaphore wait
    while (__sync_lock_test_and_set(&port->buffer_semaphore, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < 8; i++) {
        if (!__sync_lock_test_and_set(&port->buffer_locks[i], 1)) {
            __sync_lock_release(&port->buffer_semaphore);
            return i;
        }
    }
    
    __sync_lock_release(&port->buffer_semaphore);
    return -1;
}

static void ahci_port_release_buffer(ahci_port_t* port, int index) {
    if (index < 0 || index >= 8) return;
    
    __sync_lock_release(&port->buffer_locks[index]);
    __sync_lock_release(&port->buffer_semaphore);
}

static void ahci_port_lock(ahci_port_t* port) {
    while (__sync_lock_test_and_set(&port->port_lock, 1)) {
        asm volatile("pause");
    }
}

static void ahci_port_unlock(ahci_port_t* port) {
    __sync_synchronize();
    __sync_lock_release(&port->port_lock);
}

static int ahci_port_access(ahci_port_t* port, uint64_t lba, uint32_t count, 
                            uintptr_t phys_buffer, int write) {
    ahci_port_lock(port);
    
    hba_port_t* regs = port->regs;
    regs->ie = 0xFFFFFFFF;
    regs->is = 0;
    
    int slot = ahci_find_cmd_slot(regs);
    if (slot == -1) {
        tio_printf("[AHCI] Could not find command slot\n");
        ahci_port_unlock(port);
        return -1;
    }
    
    regs->serr = 0;
    regs->tfd = 0;
    
    hba_cmd_header_t* cmd_header = &port->cmd_list[slot];
    
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header->a = 0;
    cmd_header->w = write;
    cmd_header->c = 0;
    cmd_header->p = 0;
    cmd_header->prdbc = 0;
    cmd_header->pmp = 0;
    
    hba_cmd_tbl_t* cmd_tbl = port->cmd_tables[slot];
    memset(cmd_tbl, 0, sizeof(hba_cmd_tbl_t));
    
    cmd_tbl->prdt_entry[0].dba = phys_buffer & 0xFFFFFFFF;
    cmd_tbl->prdt_entry[0].dbau = (phys_buffer >> 32) & 0xFFFFFFFF;
    cmd_tbl->prdt_entry[0].dbc = port->sector_size * count - 1;
    cmd_tbl->prdt_entry[0].i = 1;
    
    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)cmd_tbl->cfis;
    memset(cmd_tbl->cfis, 0, sizeof(fis_reg_h2d_t));
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;      // Command
    cmdfis->pmport = 0; // Port multiplier
    
    if (write) {
        cmdfis->command = 0x35; // ATA_CMD_WRITE_DMA_EX
    } else {
        cmdfis->command = 0x25; // ATA_CMD_READ_DMA_EX
    }
    
    cmdfis->lba0 = lba & 0xFF;
    cmdfis->lba1 = (lba >> 8) & 0xFF;
    cmdfis->lba2 = (lba >> 16) & 0xFF;
    cmdfis->device = 1 << 6;  // LBA mode
    
    cmdfis->lba3 = (lba >> 24) & 0xFF;
    cmdfis->lba4 = (lba >> 32) & 0xFF;
    cmdfis->lba5 = (lba >> 40) & 0xFF;
    
    cmdfis->countl = count & 0xFF;
    cmdfis->counth = count >> 8;
    
    cmdfis->control = 0x8;
    
    // Wait for device ready
    uint64_t timeout = ahci_get_ticks() + 100;
    while ((regs->tfd & 0x88) && ahci_get_ticks() < timeout) { // BSY | DRQ
        asm volatile("pause");
    }
    
    if (ahci_get_ticks() >= timeout) {
        tio_printf("[AHCI] Port timeout before command\n");
        ahci_port_unlock(port);
        return -1;
    }
    
    regs->ie = 0xFFFFFFFF;
    regs->is = 0xFFFFFFFF;
    
    ahci_port_start_cmd(regs);
    regs->ci |= 1 << slot;
    
    // Wait for command completion
    timeout = ahci_get_ticks() + 200; // 200ms timeout
    while ((regs->ci & (1 << slot)) && ahci_get_ticks() < timeout) {
        if (regs->is & HBA_PXIS_TFES) {
            tio_printf("[AHCI] Task file error, SERR: %x\n", regs->serr);
            ahci_port_stop_cmd(regs);
            ahci_port_unlock(port);
            return -1;
        }
        asm volatile("pause");
    }
    
    if (ahci_get_ticks() >= timeout) {
        tio_printf("[AHCI] Command timeout\n");
        ahci_port_stop_cmd(regs);
        ahci_port_unlock(port);
        return -1;
    }
    
    ahci_port_stop_cmd(regs);
    
    if (regs->is & HBA_PXIS_TFES) {
        tio_printf("[AHCI] Task file error after command\n");
        ahci_port_unlock(port);
        return -1;
    }
    
    ahci_port_unlock(port);
    return 0;
}

static void ahci_port_identify(ahci_port_t* port) {
    hba_port_t* regs = port->regs;
    
    regs->ie = 0xFFFFFFFF;
    regs->is = 0;
    
    int slot = ahci_find_cmd_slot(regs);
    if (slot == -1) {
        tio_printf("[AHCI] No command slot for IDENTIFY\n");
        return;
    }
    
    regs->tfd = 0;
    
    hba_cmd_header_t* cmd_header = &port->cmd_list[slot];
    
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header->a = 0;
    cmd_header->w = 0;
    cmd_header->c = 0;
    cmd_header->p = 0;
    cmd_header->prdbc = 0;
    cmd_header->pmp = 0;
    
    hba_cmd_tbl_t* cmd_tbl = port->cmd_tables[slot];
    memset(cmd_tbl, 0, sizeof(hba_cmd_tbl_t));
    
    uintptr_t phys_buf = port->phys_buffers[0];
    
    cmd_tbl->prdt_entry[0].dba = phys_buf & 0xFFFFFFFF;
    cmd_tbl->prdt_entry[0].dbau = (phys_buf >> 32) & 0xFFFFFFFF;
    cmd_tbl->prdt_entry[0].dbc = 512 - 1;
    cmd_tbl->prdt_entry[0].i = 1;
    
    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)cmd_tbl->cfis;
    memset(cmd_tbl->cfis, 0, sizeof(fis_reg_h2d_t));
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->pmport = 0;
    cmdfis->command = 0xEC; // ATA_CMD_IDENTIFY
    
    cmdfis->lba0 = 0;
    cmdfis->lba1 = 0;
    cmdfis->lba2 = 0;
    cmdfis->device = 0;
    cmdfis->lba3 = 0;
    cmdfis->lba4 = 0;
    cmdfis->lba5 = 0;
    cmdfis->countl = 0;
    cmdfis->counth = 0;
    cmdfis->control = 0;
    
    // Wait for device ready
    uint64_t timeout = ahci_get_ticks() + 100;
    while ((regs->tfd & 0x88) && ahci_get_ticks() < timeout) {
        asm volatile("pause");
    }
    
    regs->ie = 0xFFFFFFFF;
    regs->is = 0xFFFFFFFF;
    
    ahci_port_start_cmd(regs);
    regs->ci |= 1 << slot;
    
    timeout = ahci_get_ticks() + 200;
    while ((regs->ci & (1 << slot)) && ahci_get_ticks() < timeout) {
        if (regs->is & HBA_PXIS_TFES) {
            tio_printf("[AHCI] IDENTIFY task file error\n");
            ahci_port_stop_cmd(regs);
            return;
        }
        asm volatile("pause");
    }
    
    ahci_port_stop_cmd(regs);
    
    if (regs->is & HBA_PXIS_TFES) {
        tio_printf("[AHCI] IDENTIFY failed\n");
        return;
    }
    
    // Parse IDENTIFY data
    uint16_t* identify = (uint16_t*)port->virt_buffers[0];
    
    // Get sector size
    if (identify[106] & (1 << 12)) {
        // Word 106 bit 12: 1 = 512 byte sectors, 0 = 4096 byte sectors
        port->sector_size = (identify[106] & (1 << 12)) ? 512 : 4096;
    } else {
        port->sector_size = 512;
    }
    
    // Get total sectors
    if (identify[83] & (1 << 10)) { // LBA48 supported
        port->supports_lba48 = 1;
        port->total_sectors = *(uint64_t*)&identify[100];
    } else {
        port->supports_lba48 = 0;
        port->total_sectors = *(uint32_t*)&identify[60];
    }
    
    tio_printf("[AHCI] Port %d: %lu sectors, %d bytes/sector, LBA48: %s\n",
               port->port_num, port->total_sectors, port->sector_size,
               port->supports_lba48 ? "yes" : "no");
}

static ahci_port_t* ahci_port_init(int port_num, hba_port_t* port_regs, hba_mem_t* hba) {
    ahci_port_t* port = (ahci_port_t*)malloc(sizeof(ahci_port_t));
    if (!port) return NULL;
    
    memset(port, 0, sizeof(ahci_port_t));
    port->regs = port_regs;
    port->port_num = port_num;
    port->status = AHCI_PORT_UNINITIALIZED;
    port->sector_size = 512;
    
    // Stop command engine
    port_regs->cmd &= ~HBA_PXCMD_ST;
    port_regs->cmd &= ~HBA_PXCMD_FRE;
    ahci_port_stop_cmd(port_regs);
    
    // Allocate command list (1K aligned)
    void* cmd_list_phys = pmm_alloc_page(&pmm);
    if (!cmd_list_phys) {
        free(port);
        return NULL;
    }
    memset(cmd_list_phys, 0, PAGE_SIZE);
    
    port_regs->clb = (uint32_t)((uintptr_t)cmd_list_phys & 0xFFFFFFFF);
    port_regs->clbu = (uint32_t)((uintptr_t)cmd_list_phys >> 32);
    port->cmd_list = (hba_cmd_header_t*)cmd_list_phys;
    
    // Allocate FIS (256 bytes)
    void* fis_phys = pmm_alloc_page(&pmm);
    if (!fis_phys) {
        free(port);
        return NULL;
    }
    memset(fis_phys, 0, PAGE_SIZE);
    
    port_regs->fb = (uint32_t)((uintptr_t)fis_phys & 0xFFFFFFFF);
    port_regs->fbu = (uint32_t)((uintptr_t)fis_phys >> 32);
    port->fis = (hba_fis_t*)fis_phys;
    
    // Initialize FIS types
    port->fis->dsfis.fis_type = FIS_TYPE_DMA_SETUP;
    port->fis->psfis.fis_type = FIS_TYPE_PIO_SETUP;
    port->fis->rfis.fis_type = FIS_TYPE_REG_D2H;
    port->fis->sdbfis[0] = FIS_TYPE_DEV_BITS;
    
    // Allocate command tables for 8 slots
    for (int i = 0; i < 8; i++) {
        void* tbl_phys = pmm_alloc_page(&pmm);
        if (!tbl_phys) {
            // Cleanup would be needed here
            free(port);
            return NULL;
        }
        memset(tbl_phys, 0, PAGE_SIZE);
        
        port->cmd_list[i].prdtl = 1;
        port->cmd_list[i].ctba = (uint32_t)((uintptr_t)tbl_phys & 0xFFFFFFFF);
        port->cmd_list[i].ctbau = (uint32_t)((uintptr_t)tbl_phys >> 32);
        port->cmd_tables[i] = (hba_cmd_tbl_t*)tbl_phys;
    }
    
    // Allocate DMA buffers
    for (int i = 0; i < 8; i++) {
        void* buf_phys = pmm_alloc_page(&pmm);
        if (!buf_phys) {
            free(port);
            return NULL;
        }
        void* buf_virt = buf_phys; // Identity mapping
        
        port->phys_buffers[i] = (uintptr_t)buf_phys;
        port->virt_buffers[i] = buf_virt;
        port->buffer_locks[i] = 0;
    }
    
    port->buffer_semaphore = 0;
    port->port_lock = 0;
    
    // Configure port
    port_regs->sctl |= (SCTL_PORT_IPM_NOPART | SCTL_PORT_IPM_NOSLUM | SCTL_PORT_IPM_NODSLP);
    
    if (hba->cap & AHCI_CAP_SALP) {
        port_regs->cmd &= ~HBA_PXCMD_ASP;
    }
    
    port_regs->is = 0;
    port_regs->ie = 1;
    
    port_regs->cmd |= HBA_PXCMD_POD;
    port_regs->cmd |= HBA_PXCMD_SUD;
    
    ahci_mdelay(10); // Wait 10ms
    
    // Check if device present
    uint64_t timeout = ahci_get_ticks() + 100;
    while ((port_regs->ssts & HBA_PXSSTS_DET) != HBA_PXSSTS_DET_PRESENT && ahci_get_ticks() < timeout) {
        asm volatile("pause");
    }
    
    if ((port_regs->ssts & HBA_PXSSTS_DET) != HBA_PXSSTS_DET_PRESENT) {
        tio_printf("[AHCI] Port %d: device not present\n", port_num);
        port->status = AHCI_PORT_ERROR;
        return port;
    }
    
    // Set interface to active
    port_regs->cmd = (port_regs->cmd & ~HBA_PXCMD_ICC) | HBA_PXCMD_ICC_ACTIVE;
    
    // Wait for device ready
    timeout = ahci_get_ticks() + 1000;
    while ((port_regs->tfd & 0x88) && ahci_get_ticks() < timeout) {
        asm volatile("pause");
    }
    
    port->status = AHCI_PORT_ACTIVE;
    
    // Identify device
    ahci_port_identify(port);
    
    tio_printf("[AHCI] Port %d initialized\n", port_num);
    return port;
}

// ==================== PUBLIC FUNCTIONS ====================

int ahci_init(void) {
    tio_printf("[AHCI] Initializing...\n");
    
    // Find AHCI controller (class 0x01, subclass 0x06? Actually SATA is class 0x01, subclass 0x06)
    // But AHCI controller itself is class 0x01, subclass 0x06? No, AHCI is usually class 0x01, subclass 0x06
    // Actually in your PCI code, SATA is class 0x01, subclass 0x06
    g_ahci_pci_dev = pci_find_class(0x01, 0x06);
    if (!g_ahci_pci_dev) {
        tio_printf("[AHCI] No controller found\n");
        return -1;
    }
    
    tio_printf("[AHCI] Found at %02X:%02X.%X\n", 
               g_ahci_pci_dev->bus, g_ahci_pci_dev->slot, g_ahci_pci_dev->function);
    
    // Enable PCI device
    pci_enable(g_ahci_pci_dev);
    pci_enable_busmaster(g_ahci_pci_dev);
    
    // Get BAR5 (AHCI base address)
    g_ahci_phys_base = g_ahci_pci_dev->bars[5] & ~0xF;
    if (!g_ahci_phys_base) {
        tio_printf("[AHCI] No BAR5\n");
        return -1;
    }
    
    // Identity map for now
    g_ahci_virt_base = g_ahci_phys_base;
    g_ahci_hba = (hba_mem_t*)g_ahci_virt_base;
    
    tio_printf("[AHCI] MMIO at 0x%lx\n", g_ahci_phys_base);
    
    // Enable AHCI
    uint32_t timeout = ahci_get_ticks() + 100;
    while (!(g_ahci_hba->ghc & AHCI_GHC_ENABLE) && ahci_get_ticks() < timeout) {
        g_ahci_hba->ghc |= AHCI_GHC_ENABLE;
        asm volatile("pause");
    }
    
    g_ahci_hba->ghc &= ~AHCI_GHC_IE;
    
    tio_printf("[AHCI] CAP: 0x%x, CAP2: 0x%x, GHCR: 0x%x\n", 
               g_ahci_hba->cap, g_ahci_hba->cap2, g_ahci_hba->ghc);
    
    // Clear interrupts
    g_ahci_hba->is = 0xFFFFFFFF;
    
    uint32_t pi = g_ahci_hba->pi;
    tio_printf("[AHCI] Implemented ports: 0x%x\n", pi);
    
    // Initialize each port
    for (int i = 0; i < 32; i++) {
        if ((pi >> i) & 1) {
            // Check if device present and active
            if (((g_ahci_hba->ports[i].ssts >> 8) & 0x0F) != 1 ||  // IPM active?
                (g_ahci_hba->ports[i].ssts & HBA_PXSSTS_DET) != HBA_PXSSTS_DET_PRESENT) {
                continue;
            }
            
            // Check signature
            uint32_t sig = g_ahci_hba->ports[i].sig;
            if (sig == SATA_SIG_ATAPI || sig == SATA_SIG_PM || sig == SATA_SIG_SEMB) {
                continue; // Not supported yet
            }
            
            // Found SATA drive
            tio_printf("[AHCI] Found SATA drive on port %d\n", i);
            
            ahci_port_t* port = ahci_port_init(i, &g_ahci_hba->ports[i], g_ahci_hba);
            if (port && port->status == AHCI_PORT_ACTIVE) {
                g_ahci_ports[i] = port;
            }
        }
    }
    
    tio_printf("[AHCI] Initialization complete\n");
    return 0;
}

void ahci_stop_cmd(hba_port_t* port) {
    ahci_port_stop_cmd(port);
}

void ahci_start_cmd(hba_port_t* port) {
    ahci_port_start_cmd(port);
}

// ==================== DISK OPERATIONS ====================

int ahci_port_read(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer) {
    if (!port || port->status != AHCI_PORT_ACTIVE) return -1;
    
    uint32_t sector_size = port->sector_size;
    uint32_t total_bytes = count;
    uint8_t* buf_ptr = (uint8_t*)buffer;
    uint64_t remaining_sectors = (count + sector_size - 1) / sector_size;
    
    int buf_idx = ahci_port_acquire_buffer(port);
    if (buf_idx < 0) return -1;
    
    uintptr_t phys_buf = port->phys_buffers[buf_idx];
    void* virt_buf = port->virt_buffers[buf_idx];
    
    while (remaining_sectors > 0) {
        uint32_t sectors_this = (remaining_sectors > 8) ? 8 : remaining_sectors;
        uint32_t bytes_this = sectors_this * sector_size;
        
        int ret = ahci_port_access(port, lba, sectors_this, phys_buf, 0);
        if (ret != 0) {
            ahci_port_release_buffer(port, buf_idx);
            return -1;
        }
        
        memcpy(buf_ptr, virt_buf, bytes_this);
        
        buf_ptr += bytes_this;
        lba += sectors_this;
        remaining_sectors -= sectors_this;
    }
    
    ahci_port_release_buffer(port, buf_idx);
    return 0;
}

int ahci_port_write(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer) {
    if (!port || port->status != AHCI_PORT_ACTIVE) return -1;
    
    uint32_t sector_size = port->sector_size;
    uint32_t total_bytes = count;
    const uint8_t* buf_ptr = (const uint8_t*)buffer;
    uint64_t remaining_sectors = (count + sector_size - 1) / sector_size;
    
    int buf_idx = ahci_port_acquire_buffer(port);
    if (buf_idx < 0) return -1;
    
    uintptr_t phys_buf = port->phys_buffers[buf_idx];
    void* virt_buf = port->virt_buffers[buf_idx];
    
    while (remaining_sectors > 0) {
        uint32_t sectors_this = (remaining_sectors > 8) ? 8 : remaining_sectors;
        uint32_t bytes_this = sectors_this * sector_size;
        
        memcpy(virt_buf, buf_ptr, bytes_this);
        
        int ret = ahci_port_access(port, lba, sectors_this, phys_buf, 1);
        if (ret != 0) {
            ahci_port_release_buffer(port, buf_idx);
            return -1;
        }
        
        buf_ptr += bytes_this;
        lba += sectors_this;
        remaining_sectors -= sectors_this;
    }
    
    ahci_port_release_buffer(port, buf_idx);
    return 0;
}

ahci_port_t* ahci_get_port(int port_num) {
    if (port_num < 0 || port_num >= 32) return NULL;
    return g_ahci_ports[port_num];
}