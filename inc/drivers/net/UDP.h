#pragma once
#include <stdint.h>
struct __attribute__((packed)) udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
};
struct __attribute__((packed)) udp_packet {
    struct udp_header hdr;
    uint8_t data[];
};
void create_udp_packet(struct udp_packet* buf, void* payload, uint64_t len, uint16_t src_prt, uint16_t dst_prt);