#include <hals/pci.h>
#include <drivers/fb.h>
#include <stdint.h>
#include <hals/net/RTL8139.h>
#include <drivers/net/UDP.h>
void create_udp_packet(struct udp_packet* buf, void* payload, uint64_t len, uint16_t src_prt, uint16_t dst_prt) {
    // 1. Calculate the total length of the UDP datagram (Header + Payload)
    uint16_t total_udp_len = sizeof(struct udp_header) + len;

    // 2. Set the ports and length in Network Byte Order (Big Endian)
    buf->hdr.src_port = __builtin_bswap16(src_prt);
    buf->hdr.dst_port = __builtin_bswap16(dst_prt);
    buf->hdr.length   = __builtin_bswap16(total_udp_len);
    buf->hdr.checksum = 0; // Keeping it 0 disables UDP checksum validation (perfectly valid for bootloaders/kernels)

    // 3. Copy the payload data safely into the continuous space right after the header
    memcpy(buf->data, payload, len);
}
