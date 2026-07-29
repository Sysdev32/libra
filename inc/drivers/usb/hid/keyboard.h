#ifndef HID_KEYBOARD_H
#define HID_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* USB HID Protocol Definitions */
#define HID_CLASS_CODE           0x03
#define HID_SUBCLASS_BOOT        0x01
#define HID_PROTOCOL_KEYBOARD    0x01

/* USB Class Requests */
#define HID_REQ_GET_REPORT       0x01
#define HID_REQ_GET_IDLE         0x02
#define HID_REQ_GET_PROTOCOL     0x03
#define HID_REQ_SET_REPORT       0x09
#define HID_REQ_SET_IDLE         0x0A
#define HID_REQ_SET_PROTOCOL     0x0B

/* HID Modifier Mask Flags */
#define KEY_MOD_LCTRL            (1 << 0)
#define KEY_MOD_LSHIFT           (1 << 1)
#define KEY_MOD_LALT             (1 << 2)
#define KEY_MOD_LMETA            (1 << 3)
#define KEY_MOD_RCTRL            (1 << 4)
#define KEY_MOD_RSHIFT           (1 << 5)
#define KEY_MOD_RALT             (1 << 6)
#define KEY_MOD_RMETA            (1 << 7)

/* Standard 8-Byte HID Boot Keyboard Input Report Format */
typedef struct {
    uint8_t modifiers;      /* Bitfield for Ctrl, Shift, Alt, Meta */
    uint8_t reserved;       /* Reserved byte (OEM use) */
    uint8_t keycodes[6];    /* Up to 6 concurrent key presses */
} __attribute__((packed)) hid_keyboard_report_t;

/* Driver State Struct */
typedef struct {
    uint8_t dev_addr;
    uint8_t ep_in;
    uint16_t max_packet_size;
    bool active;
    hid_keyboard_report_t prev_report;
} hid_keyboard_device_t;

/* Driver API Functions */
bool hid_keyboard_init(void);
void hid_keyboard_poll(void);

#endif /* HID_KEYBOARD_H */