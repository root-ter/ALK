// drv/usb/usbdev.c
#include "usbdev.h"
#include "../../../libc/string.h"
#include "../../../base/mem/mem.h"
#include "../../../base/term/term.h"
#include "../../../base/term/tio.h"

// ==================== GLOBALS ====================

static struct {
    usb_device_t*      devices;
    int                device_count;
    
    usb_controller_ops_t* controllers[8];
    void*                 controller_data[8];
    int                   controller_count;
    
    usb_class_driver_t*   class_drivers[32];
    int                   class_driver_count;
    
    term_t*                term;
} g_usb;

// ==================== HELPER FUNCTIONS ====================

static const char* usb_speed_str(int speed) {
    switch(speed) {
        case USB_SPEED_LOW:  return "Low";
        case USB_SPEED_FULL: return "Full";
        case USB_SPEED_HIGH: return "High";
        case USB_SPEED_SUPER: return "Super";
        default: return "Unknown";
    }
}

static const char* usb_class_str(uint8_t class) {
    switch(class) {
        case USB_CLASS_PER_INTERFACE:   return "Per-interface";
        case USB_CLASS_AUDIO:            return "Audio";
        case USB_CLASS_COMM:             return "Communications";
        case USB_CLASS_HID:              return "HID";
        case USB_CLASS_PHYSICAL:         return "Physical";
        case USB_CLASS_IMAGE:            return "Image";
        case USB_CLASS_PRINTER:          return "Printer";
        case USB_CLASS_MASS_STORAGE:     return "Mass Storage";
        case USB_CLASS_HUB:               return "Hub";
        case USB_CLASS_CDC_DATA:          return "CDC Data";
        case USB_CLASS_SMART_CARD:        return "Smart Card";
        case USB_CLASS_CONTENT_SECURITY:  return "Content Security";
        case USB_CLASS_VIDEO:             return "Video";
        case USB_CLASS_PERSONAL_HEALTH:   return "Personal Health";
        case USB_CLASS_WIRELESS:          return "Wireless";
        case USB_CLASS_MISC:               return "Misc";
        case USB_CLASS_APP_SPECIFIC:       return "Application Specific";
        case USB_CLASS_VENDOR_SPECIFIC:    return "Vendor Specific";
        default: return "Unknown";
    }
}

// ==================== DEVICE MANAGEMENT ====================

void usb_core_init(term_t* term) {
    memset(&g_usb, 0, sizeof(g_usb));
    g_usb.term = term;
    
    tio_printf("[USB] Core initialized\n");
}

usb_device_t* usb_device_add(void* controller_data, uint8_t port, uint8_t speed) {
    if (g_usb.device_count >= USB_MAX_DEVICES) {
        tio_printf("[USB] Too many devices\n");
        return NULL;
    }
    
    usb_device_t* dev = (usb_device_t*)malloc(sizeof(usb_device_t));
    if (!dev) return NULL;
    
    memset(dev, 0, sizeof(usb_device_t));
    
    // Basic info
    dev->port = port;
    dev->speed = speed;
    dev->connected = true;
    dev->controller_data = controller_data;
    
    // Generate name
    snprintf(dev->name, sizeof(dev->name), "usb_%d", g_usb.device_count + 1);
    
    // Add to list
    dev->next = g_usb.devices;
    g_usb.devices = dev;
    g_usb.device_count++;
    
    tio_printf("[USB] Device added on port %d (%s speed)\n", 
                port, usb_speed_str(speed));
    
    return dev;
}

void usb_device_remove(usb_device_t* dev) {
    if (!dev) return;
    
    // Find and remove from list
    usb_device_t** p = &g_usb.devices;
    while (*p) {
        if (*p == dev) {
            *p = dev->next;
            break;
        }
        p = &(*p)->next;
    }
    
    tio_printf("[USB] Device removed: %s\n", dev->name);
    free(dev);
    g_usb.device_count--;
}

// ==================== DEVICE ENUMERATION ====================

static int usb_get_descriptor(usb_device_t* dev, uint8_t type, uint8_t index,
                              void* buffer, int size) {
    return usb_control_msg(dev, USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_RECIP_DEVICE,
                          USB_REQ_GET_DESCRIPTOR, (type << 8) | index,
                          0, size, buffer, 1000);
}

int usb_device_enumerate(usb_device_t* dev) {
    tio_printf("[USB] Enumerating device %s...\n", dev->name);
    
    // 1. Get first 8 bytes of device descriptor (to get max packet size)
    uint8_t temp_desc[8];
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, temp_desc, 8) < 0) {
        tio_printerr("[USB] Failed to get device descriptor\n");
        return -1;
    }
    
    dev->device_desc.bMaxPacketSize0 = temp_desc[7];
    
    // 2. Set address
    dev->address = g_usb.device_count; // Temporary
    if (usb_control_msg(dev, USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_SET_ADDRESS, dev->address, 0, 0, NULL, 1000) < 0) {
        tio_printerr("[USB] Failed to set address\n");
        return -1;
    }
    
    // Wait for address to take effect
    for (volatile int i = 0; i < 100000; i++);
    
    // 3. Get full device descriptor
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dev->device_desc, 
                           sizeof(usb_device_desc_t)) < 0) {
        tio_printerr("[USB] Failed to get full device descriptor\n");
        return -1;
    }
    
    // Save IDs
    dev->vendor_id = dev->device_desc.idVendor;
    dev->product_id = dev->device_desc.idProduct;
    dev->bcd_device = dev->device_desc.bcdDevice;
    
    tio_printf("[USB] VID=0x%04X, PID=0x%04X, Class=%s\n",
                dev->vendor_id, dev->product_id,
                usb_class_str(dev->device_desc.bDeviceClass));
    
    // 4. Get configuration descriptor
    uint8_t config_buf[256];
    if (usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0, config_buf, 9) < 0) {
        tio_printerr("[USB] Failed to get config header\n");
        return -1;
    }
    
    uint16_t total_len = config_buf[2] | (config_buf[3] << 8);
    if (total_len > 256) total_len = 256;
    
    if (usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0, config_buf, total_len) < 0) {
        tio_printerr("[USB] Failed to get full config\n");
        return -1;
    }
    
    // Parse configuration
    memcpy(&dev->config_desc, config_buf, sizeof(usb_config_desc_t));
    
    uint8_t* ptr = config_buf + dev->config_desc.bLength;
    uint8_t* end = config_buf + total_len;
    
    while (ptr < end && dev->interface_count < USB_MAX_INTERFACES) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];
        
        if (len == 0) break;
        if (ptr + len > end) break;
        
        if (type == USB_DESC_INTERFACE) {
            usb_interface_desc_t* iface_desc = (usb_interface_desc_t*)ptr;
            usb_interface_t* iface = &dev->interfaces[dev->interface_count];
            
            iface->number = iface_desc->bInterfaceNumber;
            iface->alt_setting = iface_desc->bAlternateSetting;
            iface->class = iface_desc->bInterfaceClass;
            iface->subclass = iface_desc->bInterfaceSubClass;
            iface->protocol = iface_desc->bInterfaceProtocol;
            iface->ep_count = 0;
            
            dev->interface_count++;
        }
        else if (type == USB_DESC_ENDPOINT && dev->interface_count > 0) {
            usb_endpoint_desc_t* ep_desc = (usb_endpoint_desc_t*)ptr;
            usb_interface_t* iface = &dev->interfaces[dev->interface_count - 1];
            
            if (iface->ep_count < USB_MAX_ENDPOINTS) {
                usb_endpoint_t* ep = &iface->endpoints[iface->ep_count];
                ep->address = ep_desc->bEndpointAddress;
                ep->type = ep_desc->bmAttributes & 0x03;
                ep->max_packet = ep_desc->wMaxPacketSize;
                ep->interval = ep_desc->bInterval;
                iface->ep_count++;
            }
        }
        
        ptr += len;
    }
    
    // 5. Set configuration
    if (usb_control_msg(dev, USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_SET_CONFIGURATION, dev->config_desc.bConfigurationValue,
                        0, 0, NULL, 1000) < 0) {
        tio_printerr("[USB] Failed to set configuration\n");
        return -1;
    }
    
    dev->configuration = dev->config_desc.bConfigurationValue;
    
    tio_printf("[USB] Device %s ready, %d interfaces\n",
                dev->name, dev->interface_count);
    
    // 6. Probe class drivers
    for (int i = 0; i < dev->interface_count; i++) {
        usb_interface_t* iface = &dev->interfaces[i];
        
        for (int d = 0; d < g_usb.class_driver_count; d++) {
            usb_class_driver_t* drv = g_usb.class_drivers[d];
            
            if (drv->class_code == iface->class ||
                (drv->class_code == USB_CLASS_PER_INTERFACE && drv->probe)) {
                if (drv->probe(dev, iface) == 0) {
                    tio_printf("[USB] Driver %s claimed interface %d\n",
                                drv->name, iface->number);
                    break;
                }
            }
        }
    }
    
    return 0;
}

// ==================== CONTROLLER REGISTRATION ====================

int usb_register_controller(usb_controller_ops_t* ops, void* controller_data) {
    if (g_usb.controller_count >= 8) return -1;
    
    g_usb.controllers[g_usb.controller_count] = ops;
    g_usb.controller_data[g_usb.controller_count] = controller_data;
    g_usb.controller_count++;
    
    tio_printf("[USB] Registered controller: %s\n", ops->name);
    return g_usb.controller_count - 1;
}

void usb_unregister_controller(int controller_id) {
    if (controller_id >= 0 && controller_id < g_usb.controller_count) {
        g_usb.controllers[controller_id] = NULL;
        g_usb.controller_data[controller_id] = NULL;
    }
}

// ==================== CLASS DRIVER REGISTRATION ====================

int usb_register_class_driver(usb_class_driver_t* driver) {
    if (g_usb.class_driver_count >= 32) return -1;
    
    g_usb.class_drivers[g_usb.class_driver_count++] = driver;
    
    tio_printf("[USB] Registered class driver: %s (class=0x%02X)\n",
                driver->name, driver->class_code);
    
    // Probe existing devices
    usb_device_t* dev = g_usb.devices;
    while (dev) {
        for (int i = 0; i < dev->interface_count; i++) {
            usb_interface_t* iface = &dev->interfaces[i];
            if (iface->class == driver->class_code) {
                driver->probe(dev, iface);
            }
        }
        dev = dev->next;
    }
    
    return 0;
}

void usb_unregister_class_driver(usb_class_driver_t* driver) {
    for (int i = 0; i < g_usb.class_driver_count; i++) {
        if (g_usb.class_drivers[i] == driver) {
            // Remove from array
            for (int j = i; j < g_usb.class_driver_count - 1; j++) {
                g_usb.class_drivers[j] = g_usb.class_drivers[j + 1];
            }
            g_usb.class_driver_count--;
            break;
        }
    }
}

// ==================== TRANSFER FUNCTIONS ====================

int usb_control_msg(usb_device_t* dev, uint8_t request_type,
                   uint8_t request, uint16_t value,
                   uint16_t index, uint16_t length,
                   void* data, int timeout) {
    if (!dev || !dev->controller_data) return -1;
    
    // Find controller for this device
    for (int i = 0; i < g_usb.controller_count; i++) {
        if (g_usb.controllers[i] && g_usb.controllers[i]->control_transfer) {
            return g_usb.controllers[i]->control_transfer(
                dev, request_type, request, value, index, length, data, timeout);
        }
    }
    
    return -1;
}

int usb_bulk_read(usb_device_t* dev, uint8_t endpoint,
                 void* data, int length, int timeout) {
    if (!dev || !dev->controller_data) return -1;
    
    for (int i = 0; i < g_usb.controller_count; i++) {
        if (g_usb.controllers[i] && g_usb.controllers[i]->bulk_transfer) {
            return g_usb.controllers[i]->bulk_transfer(
                dev, endpoint | USB_DIR_IN, data, length, timeout);
        }
    }
    
    return -1;
}

int usb_bulk_write(usb_device_t* dev, uint8_t endpoint,
                  void* data, int length, int timeout) {
    if (!dev || !dev->controller_data) return -1;
    
    for (int i = 0; i < g_usb.controller_count; i++) {
        if (g_usb.controllers[i] && g_usb.controllers[i]->bulk_transfer) {
            return g_usb.controllers[i]->bulk_transfer(
                dev, endpoint & 0x7F, data, length, timeout);
        }
    }
    
    return -1;
}

// ==================== FIND DEVICES ====================

usb_device_t* usb_find_device(uint16_t vendor_id, uint16_t product_id) {
    usb_device_t* dev = g_usb.devices;
    while (dev) {
        if (dev->vendor_id == vendor_id && dev->product_id == product_id) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

usb_device_t* usb_find_class(uint8_t class_code, uint8_t subclass, int index) {
    usb_device_t* dev = g_usb.devices;
    int found = 0;
    
    while (dev) {
        if (dev->device_desc.bDeviceClass == class_code) {
            if (found == index) return dev;
            found++;
        }
        dev = dev->next;
    }
    return NULL;
}

int usb_get_device_list(usb_device_t** list, int max_count) {
    int count = 0;
    usb_device_t* dev = g_usb.devices;
    
    while (dev && count < max_count) {
        list[count++] = dev;
        dev = dev->next;
    }
    
    return count;
}

// ==================== INFORMATION FUNCTIONS ====================

void usb_get_device_info(usb_device_t* dev, char* buffer, int size) {
    if (!dev || !buffer || size == 0) return;
    
    char speed_str[16];
    switch(dev->speed) {
        case USB_SPEED_LOW:  strcpy(speed_str, "Low"); break;
        case USB_SPEED_FULL: strcpy(speed_str, "Full"); break;
        case USB_SPEED_HIGH: strcpy(speed_str, "High"); break;
        default: strcpy(speed_str, "Unknown"); break;
    }
    
    snprintf(buffer, size,
             "Device: %s\n"
             "Address: %d, Port: %d\n"
             "Speed: %s\n"
             "VID:PID = %04X:%04X\n"
             "Class: 0x%02X (%s)\n"
             "Subclass: 0x%02X, Protocol: 0x%02X\n"
             "Config: %d, Interfaces: %d\n"
             "Max Packet 0: %d",
             dev->name,
             dev->address, dev->port,
             speed_str,
             dev->vendor_id, dev->product_id,
             dev->device_desc.bDeviceClass,
             usb_class_str(dev->device_desc.bDeviceClass),
             dev->device_desc.bDeviceSubClass,
             dev->device_desc.bDeviceProtocol,
             dev->configuration,
             dev->interface_count,
             dev->device_desc.bMaxPacketSize0);
}

void usb_dump_device(usb_device_t* dev) {
    if (!dev || !g_usb.term) return;
    
    tio_printf("\n=== USB Device: %s ===\n", dev->name);
    tio_printf("  Address: %d, Port: %d\n", dev->address, dev->port);
    tio_printf("  Speed: %s\n", usb_speed_str(dev->speed));
    tio_printf("  VID: 0x%04X, PID: 0x%04X\n", dev->vendor_id, dev->product_id);
    tio_printf("  Class: 0x%02X (%s)\n", 
                dev->device_desc.bDeviceClass,
                usb_class_str(dev->device_desc.bDeviceClass));
    tio_printf("  Subclass: 0x%02X, Protocol: 0x%02X\n",
                dev->device_desc.bDeviceSubClass,
                dev->device_desc.bDeviceProtocol);
    tio_printf("  Configurations: %d\n", dev->device_desc.bNumConfigurations);
    tio_printf("  Interfaces: %d\n", dev->interface_count);
    
    for (int i = 0; i < dev->interface_count; i++) {
        usb_interface_t* iface = &dev->interfaces[i];
        tio_printf("    Interface %d:\n", iface->number);
        tio_printf("      Class: 0x%02X, Subclass: 0x%02X, Protocol: 0x%02X\n",
                    iface->class, iface->subclass, iface->protocol);
        tio_printf("      Endpoints: %d\n", iface->ep_count);
        
        for (int e = 0; e < iface->ep_count; e++) {
            usb_endpoint_t* ep = &iface->endpoints[e];
            tio_printf("        EP 0x%02X: ", ep->address);
            switch(ep->type) {
                case USB_EP_TYPE_CONTROL:    term_printf(g_usb.term, "Control"); break;
                case USB_EP_TYPE_ISOCHRONOUS: term_printf(g_usb.term, "Isochronous"); break;
                case USB_EP_TYPE_BULK:       term_printf(g_usb.term, "Bulk"); break;
                case USB_EP_TYPE_INTERRUPT:  term_printf(g_usb.term, "Interrupt"); break;
            }
            tio_printf(", MaxPacket=%d, Interval=%d\n",
                        ep->max_packet, ep->interval);
        }
    }
    
    tio_printf("========================\n\n");
}

void usb_dump_all(void) {
    if (!g_usb.term) return;
    
    tio_printf("\n=== USB Devices (%d) ===\n", g_usb.device_count);
    
    usb_device_t* dev = g_usb.devices;
    int index = 1;
    while (dev) {
        tio_printf("%d. %s: %04X:%04X (%s speed)\n",
                    index++, dev->name, dev->vendor_id, dev->product_id,
                    usb_speed_str(dev->speed));
        dev = dev->next;
    }
    
    if (g_usb.device_count == 0) {
        tio_printerr("No USB devices found\n");
    }
}