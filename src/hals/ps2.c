#include <stdint.h>
#include <stddef.h>
#include <hals/ps2.h>
static const char kbd_us_keymap[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', /* Backspace */
  '\t', /* Tab */
  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', /* Enter */
    0,  /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   
    0,  /* 42   - Left Shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   
    0,  /* 54   - Right Shift */
  '*',
    0,  /* 56   - Alt */
  ' ',  /* Space bar */
    0,  /* 58   - Caps lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 59-68 - F1 to F10 keys */
    0,  /* 69   - Num lock */
    0,  /* 70   - Scroll Lock */
    0,  /* 71   - Home key */
    0,  /* 72   - Up Arrow */
    0,  /* 73   - Page Up */
  '-',
    0,  /* 75   - Left Arrow */
    0,
    0,  /* 77   - Right Arrow */
  '+',
    0,  /* 79   - End key */
    0,  /* 80   - Down Arrow */
    0,  /* 81   - Page Down */
    0,  /* 82   - Insert Key */
    0,  /* 83   - Delete Key */
    0, 0, 0,
    0,  /* 87   - F11 Key */
    0,  /* 88   - F12 Key */
    0,  /* All others undefined */
};
volatile int last_scancode = -1;
static inline void ps2_wait_write(void) {
    while (inb(0x64) & 2);
}

// Wait until the controller has DATA for us to read (Bit 0 must be 1)
static inline void ps2_wait_read(void) {
    while (!(inb(0x64) & 1));
}
void ps2_enable_mouse(void) {
    // Enable PS/2 mouse port 2 and turn on data reporting.
    ps2_wait_write();
    outb(0x64, 0xA8); // Enable Port 2 (Mouse)

    ps2_wait_write();
    outb(0x64, 0xA9); // Test Port 2
    ps2_wait_read();
    if (inb(0x60) != 0x00) {
        return; // No mouse or no PS/2 port 2 support.
    }

    // Enable mouse interrupts in the controller configuration byte.
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    uint8_t config = inb(0x60);
    config |= (1 << 1); // Enable IRQ12 for port 2

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, config);

    // Select default mouse packet mode.
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xF6);
    ps2_wait_read();
    if (inb(0x60) != 0xFA) {
        return;
    }

    // Enable mouse streaming / data reporting.
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xF4);
    ps2_wait_read();
    if (inb(0x60) != 0xFA) {
        return;
    }
}

void ps2_init(void) {
    // 1. Disable both PS/2 channels (Port 1 and Port 2) so they don't flood us with data
    ps2_wait_write();
    outb(0x64, 0xAD); // Disable Port 1 (Keyboard)
    ps2_wait_write();
    outb(0x64, 0xA7); // Disable Port 2 (Mouse - okay if it doesn't exist)

    // 2. Flush the buffer (Read any leftover garbage data out of port 0x60)
    while (inb(0x64) & 1) {
        inb(0x60);
    }

    // 3. Read the Configuration Byte
    ps2_wait_write();
    outb(0x64, 0x20); // Command: Read Controller Configuration
    ps2_wait_read();
    uint8_t config = inb(0x60);

    // 4. Modify the Configuration Byte:
    // Clear Bit 0 (Disable Port 1 Interrupts)
    // Clear Bit 1 (Disable Port 2 Interrupts)
    // Clear Bit 6 (Disable Port 1 Translation - keeps scan codes predictable)
    config &= ~(1 << 0);
    config &= ~(1 << 1);
    config &= ~(1 << 6);

    // Write the modified Configuration Byte back
    ps2_wait_write();
    outb(0x64, 0x60); // Command: Write Controller Configuration
    ps2_wait_write();
    outb(0x60, config);

    // 5. Perform Controller Self-Test
    ps2_wait_write();
    outb(0x64, 0xAA); // Command: Test Controller
    ps2_wait_read();
    if (inb(0x60) != 0x55) {
        // Controller is broken or missing!
        return;
    }

    // 6. Perform Interface Test (Port 1)
    ps2_wait_write();
    outb(0x64, 0xAB); // Command: Test Port 1
    ps2_wait_read();
    if (inb(0x60) != 0x00) {
        // Keyboard interface test failed
        return;
    }

    // 7. Enable the Keyboard Port and Turn on Interrupts
    ps2_wait_write();
    outb(0x64, 0xAE); // Command: Enable Port 1

    // Re-read configuration byte to flip the interrupt bit back on
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    config = inb(0x60);

    config |= (1 << 0); // Set Bit 0: Enable Port 1 Interrupt (IRQ 1)

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, config);

    // 8. Reset the actual Keyboard hardware device
    ps2_wait_write();
    outb(0x60, 0xFF); // Device Command: Reset
    ps2_wait_read();
    if (inb(0x60) == 0xFA) { // ACK (Acknowledge)
        ps2_wait_read();
        uint8_t self_test_res = inb(0x60); // Should be 0xAA for passed
        (void)self_test_res;
    }

    // 9. Enable the PS/2 mouse hardware on port 2.
    ps2_enable_mouse();
}
char kgetc() {
    // Wait for ISR to register a new press
    while (last_scancode == -1 && (!(last_scancode & 0x80))) {
        __asm__ volatile("hlt"); // Save CPU power while waiting
    }

    uint8_t code = last_scancode;
    last_scancode = -1; // Clear the latch

    return kbd_us_keymap[last_scancode];
}
void keyboard_dispatch() {
    uint8_t scancode = inb(0x60);
    last_scancode = scancode;
    
    // Virtual Console Switching via Function Keys (F1 - F6)
    if (scancode == 0x3B) {
        tty_switch(0);
    } else if (scancode == 0x3C) {
        tty_switch(1);
    } else if (scancode == 0x3D) {
        tty_switch(2);
    } else if (scancode == 0x3E) {
        tty_switch(3);
    } else if (scancode == 0x3F) {
        tty_switch(4);
    } else if (scancode == 0x40) {
        tty_switch(5);
    }
    
    // Ensure it's a make code (press event)
    if (!(scancode & 0x80)) {
        char c = kbd_us_keymap[last_scancode];
        
        // Only forward standard printable character keys down to the TTY sub-system
        if (c >= ' ' && c <= '~') {
            tty_handle_input(c);
        }
    }
}

static volatile kbd_buffer_t kbd_buf = { .head = 0, .tail = 0 };
static volatile mouse_buffer_t mouse_buf = { .head = 0, .tail = 0 };
static uint8_t mouse_packet[3];
static int mouse_phase = 0;

// Call this inside your keyboard interrupt handler to save a scancode
void kbd_buffer_push(uint8_t scancode) {
    uint32_t next = (kbd_buf.head + 1) % KBD_BUFFER_SIZE;
    
    // If the buffer is full, discard the scancode to protect kernel memory
    if (next == kbd_buf.tail) {
        return;
    }
    
    kbd_buf.data[kbd_buf.head] = scancode;
    kbd_buf.head = next;
}

// This function checks if user space has anything to read
int kbd_buffer_pop(uint8_t *out_scancode) {
    if (kbd_buf.head == kbd_buf.tail) {
        return 0; // Buffer is empty
    }
    
    *out_scancode = kbd_buf.data[kbd_buf.tail];
    kbd_buf.tail = (kbd_buf.tail + 1) % KBD_BUFFER_SIZE;
    return 1; // Successfully popped a scancode
}

void mouse_buffer_push(mouse_event_t event) {
    uint32_t next = (mouse_buf.head + 1) % MOUSE_BUFFER_SIZE;
    if (next == mouse_buf.tail) {
        return; // Buffer full, drop oldest mouse event to preserve kernel stability
    }
    mouse_buf.data[mouse_buf.head] = event;
    mouse_buf.head = next;
}

int mouse_buffer_pop(mouse_event_t *out_event) {
    if (mouse_buf.head == mouse_buf.tail) {
        return 0;
    }
    *out_event = mouse_buf.data[mouse_buf.tail];
    mouse_buf.tail = (mouse_buf.tail + 1) % MOUSE_BUFFER_SIZE;
    return 1;
}
// System call wrapper exposed to your interrupt/syscall table
int sys_read_key(uint8_t *user_buffer) {
    uint8_t scancode;
    
    // Attempt to pull a key out of the queue
    if (kbd_buffer_pop(&scancode)) {
        // Safely write the byte to the user space pointer address
        *user_buffer = scancode;
        return 1; // Success
    }
    
    return 0; // No keys available right now
}

int read_mouse(void *user_buffer, uint64_t count) {
    if (user_buffer == NULL || count < sizeof(mouse_event_t)) {
        return -1;
    }

    mouse_event_t event;
    while (!mouse_buffer_pop(&event)) {
        asm volatile("sti; hlt" ::: "memory");
    }

    mouse_event_t *dest = (mouse_event_t*)user_buffer;
    *dest = event;
    return sizeof(mouse_event_t);
}
void mouse_dispatch() {
	uint8_t byte = inb(0x60);

	if (mouse_phase == 0) {
		// Synchronize to the first byte: bit 3 must be set in a valid PS/2 packet.
		if (!(byte & 0x08)) {
			return 0;
		}
	}

	mouse_packet[mouse_phase++] = byte;
	if (mouse_phase < 3) {
		lapic_eoi();
		return 0;
	}

	mouse_phase = 0;
	mouse_event_t event = {
		.dx = (int8_t)mouse_packet[1],
		.dy = (int8_t)mouse_packet[2],
		.buttons = mouse_packet[0] & 0x07,
		.reserved = 0,
	};
	mouse_buffer_push(event);
}
static inline void ps2_wait_input(void)
{
    while (inb(0x64) & 0x02)
        ;
}

static inline void ps2_wait_output(void)
{
    while (!(inb(0x64) & 0x01))
        ;
}

static inline void ps2_flush(void)
{
    while (inb(0x64) & 0x01) {
        uint8_t b = inb(0x60);
    }
}

static inline uint8_t ps2_read_config(void)
{
    ps2_wait_input();
    outb(0x64, 0x20);

    ps2_wait_output();
    return inb(0x60);
}

static void ps2_write_config(uint8_t config)
{
    ps2_wait_input();
    outb(0x64, 0x60);

    ps2_wait_input();
    outb(0x60, config);
}

static uint8_t ps2_send_device(uint8_t cmd)
{
    ps2_wait_input();
    outb(0x60, cmd);

    ps2_wait_output();
    return inb(0x60);
}

void keyboard_init(void)
{
    uint8_t config;
    ps2_flush();
    ps2_wait_input();
    outb(0x64, 0xAD);

    /* Read config */
    config = ps2_read_config();

    /* Enable IRQ1 + enable first port clock */
    config |= (1 << 0);
    config &= ~(1 << 4);

    ps2_write_config(config);

    /* Verify config actually changed */
    config = ps2_read_config();
    ps2_wait_input();
    outb(0x64, 0xAE);

    /* Keyboard reset */

    uint8_t resp = ps2_send_device(0xFF);

    ps2_wait_output();
    resp = inb(0x60);

    /* Read keyboard ID */

    resp = ps2_send_device(0xF2);

    while (inb(0x64) & 1) {
        uint8_t id = inb(0x60);
    }
    /* Select Scan Code Set */

    /* Send F0 = Select scan code set */
    resp = ps2_send_device(0xF0);

    if (resp == 0xFA) {
        /* Send 01 = Set 1 */
        resp = ps2_send_device(0x01);
    }
    /* Enable scanning */
    resp = ps2_send_device(0xF4);
}