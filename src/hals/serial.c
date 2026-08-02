//
// Created by adam on 8/3/26.

#include <stdint.h>
#include <hals/serial.h>

void serial_write_char(char ch) {
    uint8_t status;
    do {
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
    } while ((status & 0x20) == 0);
    asm volatile("outb %0, %1" :: "a"((uint8_t)ch), "Nd"((uint16_t)0x3F8));
}