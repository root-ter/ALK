// drv/usb/usbdev.h
#ifndef USBDEV_H
#define USBDEV_H

#include <stdint.h>
#include <stdbool.h>
#include "../../../base/term/term.h"

// Максимальное количество USB устройств
#define USB_MAX_DEVICES 32
#define USB_MAX_INTERFACES 8
#define USB_MAX_ENDPOINTS 16
#define USB_DEVICE_NAME_LEN 32

// ==================== USB CONSTANTS ====================

// Device classes
#define USB_CLASS_PER_INTERFACE   0x00
#define USB_CLASS_AUDIO            0x01
#define USB_CLASS_COMM             0x02
#define USB_CLASS_HID              0x03
#define USB_CLASS_PHYSICAL         0x05
#define USB_CLASS_IMAGE            0x06
#define USB_CLASS_PRINTER          0x07
#define USB_CLASS_MASS_STORAGE     0x08
#define USB_CLASS_HUB              0x09
#define USB_CLASS_CDC_DATA         0x0A
#define USB_CLASS_SMART_CARD       0x0B
#define USB_CLASS_CONTENT_SECURITY 0x0D
#define USB_CLASS_VIDEO            0x0E
#define USB_CLASS_PERSONAL_HEALTH  0x0F
#define USB_CLASS_AUDIO_VIDEO      0x10
#define USB_CLASS_BILLBOARD        0x11
#define USB_CLASS_USB_TYPE_C       0x12
#define USB_CLASS_WIRELESS         0xE0
#define USB_CLASS_MISC             0xEF
#define USB_CLASS_APP_SPECIFIC     0xFE
#define USB_CLASS_VENDOR_SPECIFIC  0xFF

// Descriptor types
#define USB_DESC_DEVICE            0x01
#define USB_DESC_CONFIGURATION     0x02
#define USB_DESC_STRING            0x03
#define USB_DESC_INTERFACE         0x04
#define USB_DESC_ENDPOINT          0x05
#define USB_DESC_DEVICE_QUALIFIER  0x06
#define USB_DESC_OTHER_SPEED       0x07
#define USB_DESC_INTERFACE_POWER   0x08
#define USB_DESC_OTG               0x09
#define USB_DESC_DEBUG             0x0A
#define USB_DESC_INTERFACE_ASSOC   0x0B
#define USB_DESC_BOS               0x0F
#define USB_DESC_DEVICE_CAPABILITY 0x10
#define USB_DESC_SUPERSPEED_USB    0x2A
#define USB_DESC_SUPERSPEEDPLUS    0x2B

// Standard requests
#define USB_REQ_GET_STATUS         0x00
#define USB_REQ_CLEAR_FEATURE      0x01
#define USB_REQ_SET_FEATURE        0x03
#define USB_REQ_SET_ADDRESS        0x05
#define USB_REQ_GET_DESCRIPTOR     0x06
#define USB_REQ_SET_DESCRIPTOR     0x07
#define USB_REQ_GET_CONFIGURATION  0x08
#define USB_REQ_SET_CONFIGURATION  0x09
#define USB_REQ_GET_INTERFACE      0x0A
#define USB_REQ_SET_INTERFACE      0x0B
#define USB_REQ_SYNCH_FRAME        0x0C

// Endpoint types
#define USB_EP_TYPE_CONTROL        0x00
#define USB_EP_TYPE_ISOCHRONOUS    0x01
#define USB_EP_TYPE_BULK           0x02
#define USB_EP_TYPE_INTERRUPT      0x03

// Endpoint directions
#define USB_DIR_OUT                0x00
#define USB_DIR_IN                 0x80

// Speeds
#define USB_SPEED_LOW               0
#define USB_SPEED_FULL              1
#define USB_SPEED_HIGH              2
#define USB_SPEED_SUPER             3

// Request types
#define USB_REQ_TYPE_STANDARD    (0x00 << 5)
#define USB_REQ_TYPE_CLASS       (0x01 << 5)
#define USB_REQ_TYPE_VENDOR      (0x02 << 5)
#define USB_REQ_TYPE_RESERVED    (0x03 << 5)

// Request recipients
#define USB_RECIP_DEVICE         0x00
#define USB_RECIP_INTERFACE      0x01
#define USB_RECIP_ENDPOINT       0x02
#define USB_RECIP_OTHER          0x03

// Endpoint directions
#define USB_DIR_OUT              0x00
#define USB_DIR_IN               0x80

// Standard requests
#define USB_REQ_GET_STATUS       0x00
#define USB_REQ_CLEAR_FEATURE    0x01
#define USB_REQ_SET_FEATURE      0x03
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_DESCRIPTOR   0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SYNCH_FRAME       0x0C

// Descriptor types
#define USB_DESC_DEVICE           0x01
#define USB_DESC_CONFIGURATION    0x02
#define USB_DESC_STRING           0x03
#define USB_DESC_INTERFACE        0x04
#define USB_DESC_ENDPOINT         0x05
#define USB_DESC_DEVICE_QUALIFIER 0x06
#define USB_DESC_OTHER_SPEED      0x07
#define USB_DESC_INTERFACE_POWER  0x08
#define USB_DESC_OTG              0x09
#define USB_DESC_DEBUG            0x0A
#define USB_DESC_INTERFACE_ASSOC  0x0B
#define USB_DESC_BOS              0x0F
#define USB_DESC_DEVICE_CAPABILITY 0x10
#define USB_DESC_SUPERSPEED_USB   0x2A
#define USB_DESC_SUPERSPEEDPLUS   0x2B

// Device classes
#define USB_CLASS_PER_INTERFACE   0x00
#define USB_CLASS_AUDIO            0x01
#define USB_CLASS_COMM             0x02
#define USB_CLASS_HID              0x03
#define USB_CLASS_PHYSICAL         0x05
#define USB_CLASS_IMAGE            0x06
#define USB_CLASS_PRINTER          0x07
#define USB_CLASS_MASS_STORAGE     0x08
#define USB_CLASS_HUB              0x09
#define USB_CLASS_CDC_DATA         0x0A
#define USB_CLASS_SMART_CARD       0x0B
#define USB_CLASS_CONTENT_SECURITY 0x0D
#define USB_CLASS_VIDEO            0x0E
#define USB_CLASS_PERSONAL_HEALTH  0x0F
#define USB_CLASS_AUDIO_VIDEO      0x10
#define USB_CLASS_BILLBOARD        0x11
#define USB_CLASS_USB_TYPE_C       0x12
#define USB_CLASS_WIRELESS         0xE0
#define USB_CLASS_MISC             0xEF
#define USB_CLASS_APP_SPECIFIC     0xFE
#define USB_CLASS_VENDOR_SPECIFIC  0xFF

// Endpoint types
#define USB_EP_TYPE_CONTROL        0x00
#define USB_EP_TYPE_ISOCHRONOUS    0x01
#define USB_EP_TYPE_BULK           0x02
#define USB_EP_TYPE_INTERRUPT      0x03

// Speeds
#define USB_SPEED_LOW               0
#define USB_SPEED_FULL              1
#define USB_SPEED_HIGH              2
#define USB_SPEED_SUPER             3

// ==================== DESCRIPTOR STRUCTURES ====================

// Device descriptor
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

// Configuration descriptor
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

// Interface descriptor
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

// Endpoint descriptor
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

// String descriptor
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bString[];
} __attribute__((packed)) usb_string_desc_t;

// ==================== USB DEVICE STRUCTURES ====================

// Forward declaration
typedef struct usb_device usb_device_t;

// Endpoint structure
typedef struct {
    uint8_t  address;        // Endpoint address (with direction)
    uint8_t  type;           // Control, bulk, interrupt, isochronous
    uint16_t max_packet;     // Max packet size
    uint8_t  interval;       // Polling interval (frames)
} usb_endpoint_t;

// Interface structure
typedef struct {
    uint8_t  number;         // Interface number
    uint8_t  alt_setting;    // Alternate setting
    uint8_t  class;          // Interface class
    uint8_t  subclass;       // Interface subclass
    uint8_t  protocol;       // Interface protocol
    uint8_t  ep_count;       // Number of endpoints
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    void*    driver_data;    // Data for class driver
} usb_interface_t;

// Main USB device structure
struct usb_device {
    // Identification
    char     name[USB_DEVICE_NAME_LEN];
    uint8_t  address;
    uint8_t  port;
    uint8_t  speed;          // 0=low, 1=full, 2=high
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    
    // Descriptors
    usb_device_desc_t device_desc;
    usb_config_desc_t config_desc;
    usb_interface_t   interfaces[USB_MAX_INTERFACES];
    int               interface_count;
    
    // State
    uint8_t  configuration;
    bool     connected;
    void*    driver_data;    // Data for device driver
    
    // Controller-specific data (opaque)
    void*    controller_data;
    
    // Linked list
    struct usb_device* next;
};

// ==================== USB TRANSFER ====================

// Transfer types
#define USB_TRANSFER_CONTROL    0
#define USB_TRANSFER_BULK       1
#define USB_TRANSFER_INTERRUPT  2
#define USB_TRANSFER_ISOCHRONOUS 3

// Transfer flags
#define USB_FLAG_NONE           0
#define USB_FLAG_SHORT_OK       (1 << 0)
#define USB_FLAG_FORCE_SHORT    (1 << 1)

// Transfer status
#define USB_STATUS_OK           0
#define USB_STATUS_PENDING      1
#define USB_STATUS_ERROR        -1
#define USB_STATUS_TIMEOUT      -2
#define USB_STATUS_STALL        -3
#define USB_STATUS_NAK          -4

// USB transfer structure
typedef struct {
    usb_device_t* dev;
    uint8_t       type;
    uint8_t       endpoint;
    uint8_t       direction;
    void*         buffer;
    uint32_t      length;
    uint32_t      actual_length;
    int           status;
    void*         controller_private;
} usb_transfer_t;

// ==================== CONTROLLER OPERATIONS ====================

// Controller operations (implemented by specific drivers like EHCI, OHCI, UHCI, xHCI)
typedef struct {
    const char* name;
    
    // Control transfer
    int (*control_transfer)(usb_device_t* dev, uint8_t bmRequestType,
                           uint8_t bRequest, uint16_t wValue,
                           uint16_t wIndex, uint16_t wLength,
                           void* data, int timeout_ms);
    
    // Bulk transfer
    int (*bulk_transfer)(usb_device_t* dev, uint8_t endpoint,
                        void* data, int length, int timeout_ms);
    
    // Interrupt transfer (async)
    int (*interrupt_transfer)(usb_device_t* dev, uint8_t endpoint,
                             void* data, int length,
                             void (*callback)(usb_transfer_t*));
    
    // Reset device
    int (*reset_device)(usb_device_t* dev);
    
    // Controller info
    void (*print_info)(void);
} usb_controller_ops_t;

// ==================== CLASS DRIVER ====================

// Class driver structure
typedef struct {
    const char* name;
    uint8_t     class_code;
    uint8_t     subclass;
    uint8_t     protocol;
    
    // Called when device is connected
    int (*probe)(usb_device_t* dev, usb_interface_t* iface);
    
    // Called when device is disconnected
    void (*disconnect)(usb_device_t* dev, usb_interface_t* iface);
} usb_class_driver_t;

// ==================== USB CORE API ====================

// Initialization
void usb_core_init(term_t* term);

// Register/unregister controller
int usb_register_controller(usb_controller_ops_t* ops, void* controller_data);
void usb_unregister_controller(int controller_id);

// Register/unregister class driver
int usb_register_class_driver(usb_class_driver_t* driver);
void usb_unregister_class_driver(usb_class_driver_t* driver);

// Device enumeration (called by controller)
usb_device_t* usb_device_add(void* controller_data, uint8_t port, uint8_t speed);
void usb_device_remove(usb_device_t* dev);
int usb_device_enumerate(usb_device_t* dev);

// Find devices
usb_device_t* usb_find_device(uint16_t vendor_id, uint16_t product_id);
usb_device_t* usb_find_class(uint8_t class_code, uint8_t subclass, int index);
int usb_get_device_list(usb_device_t** list, int max_count);

// Device information
void usb_get_device_info(usb_device_t* dev, char* buffer, int size);
void usb_print_device_tree(void);

// High-level transfer functions (controller-independent)
int usb_control_msg(usb_device_t* dev, uint8_t request_type,
                   uint8_t request, uint16_t value,
                   uint16_t index, uint16_t length,
                   void* data, int timeout);

int usb_bulk_read(usb_device_t* dev, uint8_t endpoint,
                 void* data, int length, int timeout);

int usb_bulk_write(usb_device_t* dev, uint8_t endpoint,
                  void* data, int length, int timeout);

int usb_interrupt_read(usb_device_t* dev, uint8_t endpoint,
                      void* data, int length,
                      void (*callback)(usb_transfer_t*));

// String descriptors
int usb_get_string(usb_device_t* dev, uint8_t index, char* buffer, int size);

// Configuration
int usb_set_configuration(usb_device_t* dev, int configuration);
int usb_claim_interface(usb_device_t* dev, int interface);
int usb_release_interface(usb_device_t* dev, int interface);

// Debug
void usb_dump_device(usb_device_t* dev);
void usb_dump_all(void);

#endif // USBDEV_H