// drv/usb/ehci.h
#ifndef EHCI_H
#define EHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "../../../base/term/term.h"
#include "../../../base/mem/pmm.h"
#include "../usbdev/usbdev.h"

bool ehci_init(term_t* term, pmm_t* pmm);

#endif