#include <drivers/usb/hid/keyboard.h>
#include <hals/xhci.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <drivers/fb.h>
#include <drivers/fb.h>

static hid_keyboard_device_t g_keyboard = {0};

/* ============================================================================
 * HID SCANCODE TO ASCII LOOKUP TABLES
 * ============================================================================ */

static const char hid_ascii_map[256] = {
    0, 0, 0, 0,
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 0x1B, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`',
    ',', '.', '/', 0 /* CapsLock */
};

static const char hid_ascii_shift_map[256] = {
    0, 0, 0, 0,
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0x1B, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~',
    '<', '>', '?', 0
};

/* ============================================================================
 * KEY EVENT HANDLERS
 * ============================================================================ */

static void hid_keyboard_on_key_event(uint8_t scancode, uint8_t modifiers, bool pressed) {
    if (!pressed) return; /* Handling key-down events */

    bool shift = (modifiers & KEY_MOD_LSHIFT) || (modifiers & KEY_MOD_RSHIFT);
    char ascii = shift ? hid_ascii_shift_map[scancode] : hid_ascii_map[scancode];

    if (ascii) {
        if (echoison()) {
            printk(LOG_NONE, "%c", ascii);
        }
    } else {
        if (scancode == 0x3A) {
            tty_switch(0);
        } else if (scancode == 0x3B) {
            tty_switch(1);
        } else if (scancode == 0x3C) {
            tty_switch(2);
        } else if (scancode == 0x3D) {
            tty_switch(3);
        } else if (scancode == 0x3E) {
            tty_switch(4);
        } else if (scancode == 0x3F) {
            tty_switch(5);
        }
    }
}

/* Processes changes between current report and previous report */
static void hid_keyboard_process_report(hid_keyboard_report_t* report) {
    /* Handle Key Releases: Keys in prev_report not present in new report */
    for (int i = 0; i < 6; i++) {
        uint8_t old_code = g_keyboard.prev_report.keycodes[i];
        if (old_code <= 3) continue; /* Ignore NO_EVENT / ErrorRollOver */

        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (report->keycodes[j] == old_code) {
                still_pressed = true;
                break;
            }
        }

        if (!still_pressed) {
            hid_keyboard_on_key_event(old_code, report->modifiers, false);
        }
    }

    /* Handle Key Presses: Keys in new report not present in prev_report */
    for (int i = 0; i < 6; i++) {
        uint8_t new_code = report->keycodes[i];
        if (new_code <= 3) continue;

        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (g_keyboard.prev_report.keycodes[j] == new_code) {
                was_pressed = true;
                break;
            }
        }

        if (!was_pressed) {
            hid_keyboard_on_key_event(new_code, report->modifiers, true);
        }
    }

    /* Cache current state */
    g_keyboard.prev_report = *report;
}

/* ============================================================================
 * PROTOCOL SETUP & DRIVER INITIALIZATION
 * ============================================================================ */

static bool hid_set_boot_protocol(uint8_t slot_id) {
    usb_setup_packet_t setup;
    setup.request_type = 0x21; /* Host-to-Device | Class | Interface */
    setup.request      = HID_REQ_SET_PROTOCOL;
    setup.value        = 0;    /* 0 = Boot Protocol */
    setup.index        = 0;    /* Interface 0 */
    setup.length       = 0;

    return xhci_control_transfer(slot_id, &setup, NULL);
}

static bool hid_set_idle(uint8_t slot_id) {
    usb_setup_packet_t setup;
    setup.request_type = 0x21;
    setup.request      = HID_REQ_SET_IDLE;
    setup.value        = 0;    /* Duration = 0 (Report only on state change) */
    setup.index        = 0;
    setup.length       = 0;

    return xhci_control_transfer(slot_id, &setup, NULL);
}

bool hid_keyboard_init(void) {
    usb_matched_device_t matches[4];
    int count = xhci_find_devices_by_class(HID_CLASS_CODE, matches, 4);

    if (count == 0) {
        printk(LOG_ERROR, "HID Keyboard: No HID devices found on xHCI bus.\n");
        return false;
    }

    for (int i = 0; i < count; i++) {
        /* Ensure it's a Boot Protocol Keyboard */
        if (matches[i].device_subclass == HID_SUBCLASS_BOOT &&
            matches[i].device_protocol == HID_PROTOCOL_KEYBOARD) {

            g_keyboard.dev_addr = matches[i].address; /* In xHCI, dev_addr maps to Slot ID */

            /* In USB HID Boot Protocol, Endpoint 1 IN (0x81) is standard */
            g_keyboard.ep_in = 0x81;
            g_keyboard.max_packet_size = 8;

            /* 1. Configure Endpoint in xHCI Controller (Allocates EP Transfer Ring & Context) */
            if (!xhci_configure_endpoint(g_keyboard.dev_addr, g_keyboard.ep_in, g_keyboard.max_packet_size)) {
                printk(LOG_ERROR, "HID Keyboard: Failed to configure EP 0x%02X on Slot %d!\n",
                       g_keyboard.ep_in, g_keyboard.dev_addr);
                return false;
            }

            /* 2. Configure Keyboard to Boot Protocol Mode */
            hid_set_boot_protocol(g_keyboard.dev_addr);
            hid_set_idle(g_keyboard.dev_addr);

            g_keyboard.active = true;
            memset(&g_keyboard.prev_report, 0, sizeof(hid_keyboard_report_t));

            printk(LOG_INFO, "HID Keyboard: Bound to xHCI Slot %d on Endpoint 0x%02X!\n",
                   g_keyboard.dev_addr, g_keyboard.ep_in);

            return true;
        }
    }

    printk(LOG_ERROR, "HID Keyboard: No boot-compatible USB keyboard found.\n");
    return false;
}

/* ============================================================================
 * PERIODIC DRIVER POLLING
 * ============================================================================ */

void hid_keyboard_poll(void) {
    if (!g_keyboard.active) return;

    hid_keyboard_report_t report;
    memset(&report, 0, sizeof(report));

    /* Issue transfer request to xHCI event/transfer rings */
    bool status = xhci_interrupt_transfer(g_keyboard.dev_addr,
                                          g_keyboard.ep_in,
                                          &report,
                                          sizeof(hid_keyboard_report_t));

    if (status) {
        hid_keyboard_process_report(&report);
    }
}