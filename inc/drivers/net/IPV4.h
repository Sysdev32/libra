#pragma once
#include <stdint.h>
struct __attribute__((packed)) ipv4_header {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;

    uint16_t identification;
    uint16_t flags_fragment;

    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;

    uint32_t src_ip;
    uint32_t dst_ip;
};
struct __attribute__((packed)) ipv4_packet {
    struct ipv4_header hdr;
    uint8_t payload[];
};
void set_ip(uint32_t ips);
uint32_t get_ip();
void send_ipv4_packet(uint32_t dest_ip, uint8_t* payload, int protocol, int len);
#define BROADCAST 0xFFFFFFFFU