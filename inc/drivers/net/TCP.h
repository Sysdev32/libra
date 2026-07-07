#pragma once
#include <stdint.h>

struct __attribute__((packed)) tcp_header {
    uint16_t src_port;       // Source port (e.g., random high port 52001)
    uint16_t dst_port;       // Destination port (e.g., 80 for HTTP, 23 for Telnet)
    uint32_t seq_num;        // Sequence number
    uint32_t ack_num;        // Acknowledgment number
    uint8_t  data_offset;    // Data offset (Header length) / Reserved bits
    uint8_t  flags;          // TCP flags control bits
    uint16_t window_size;    // Size of the receive window buffer
    uint16_t checksum;       // Checksum field
    uint16_t urgent_ptr;     // Urgent pointer
};

struct __attribute__((packed)) tcp_packet {
    struct tcp_header hdr;
    uint8_t data[];
};

// TCP Control Flag Bitmasks
#define TCP_FLAG_FIN  (1 << 0)
#define TCP_FLAG_SYN  (1 << 1)
#define TCP_FLAG_RST  (1 << 2)
#define TCP_FLAG_PSH  (1 << 3)
#define TCP_FLAG_ACK  (1 << 4)
#define TCP_FLAG_URG  (1 << 5)

// Function declarations
void create_tcp_packet(struct tcp_packet* buf, void* payload, uint32_t len,
                       uint16_t src_port, uint16_t dst_port, 
                       uint32_t seq, uint32_t ack, uint8_t flags);