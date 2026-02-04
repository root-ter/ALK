#ifndef IO_H
#define IO_H
#include <stdint.h>
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t data);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t data);
uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t data);

static inline uint8_t mmio_read8(volatile void* addr) {
    return *(volatile uint8_t*)addr;
}

static inline uint16_t mmio_read16(volatile void* addr) {
    return *(volatile uint16_t*)addr;
}

static inline uint32_t mmio_read32(volatile void* addr) {
    return *(volatile uint32_t*)addr;
}

static inline uint64_t mmio_read64(volatile void* addr) {
    return *(volatile uint64_t*)addr;
}

static inline void mmio_write8(volatile void* addr, uint8_t value) {
    *(volatile uint8_t*)addr = value;
}

static inline void mmio_write16(volatile void* addr, uint16_t value) {
    *(volatile uint16_t*)addr = value;
}

static inline void mmio_write32(volatile void* addr, uint32_t value) {
    *(volatile uint32_t*)addr = value;
}

static inline void mmio_write64(volatile void* addr, uint64_t value) {
    *(volatile uint64_t*)addr = value;
}

#define mmio_read(addr) mmio_read32(addr)
#define mmio_write(addr, value) mmio_write32(addr, value)
#endif
