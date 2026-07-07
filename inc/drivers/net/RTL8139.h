#pragma once
#include <drivers/pci.h>
#include <drivers/ahci.h>
#include <drivers/fb.h>
#include <stdbool.h>
struct __attribute__((packed)) eth_frame {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
    uint8_t payload[]; 
};
bool rtl8139_send(const void *packet, uint16_t len);
void rtl8139_receive(uint8_t*);
void rtl8139_poll(uint8_t*);
void init_rtl8139(void);
uint8_t* get_mac();