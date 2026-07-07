#pragma once
#include <drivers/pci.h>
#include <drivers/ahci.h>
#include <drivers/fb.h>
#include <stdbool.h>
bool rtl8139_send(const void *packet, uint16_t len);
void rtl8139_receive(uint8_t*);
void rtl8139_poll(uint8_t*);
void init_rtl8139(void);