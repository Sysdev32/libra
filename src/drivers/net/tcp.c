#include <drivers/net/IPV4.h>
#include <drivers/net/TCP.h>
#include <drivers/fb.h>
#include <stdint.h>
#include <string.h>

// Internal structure used strictly for calculating 16-bit internet checksum values
struct __attribute__((packed)) tcp_pseudo_header {
    uint32_t src_ip;      // Origin local interface IP address
    uint32_t dst_ip;      // Intended outbound destination target IP
    uint8_t  reserved;    // Explicitly zeroed (0x00)
    uint8_t  protocol;    // Always 6 for TCP
    uint16_t tcp_length;  // Total size of the TCP Header + Data Payload combined
};

// External reference to find our current bound local IP (defined inside your ipv4.c)
extern uint32_t ip; 

/**
 * Calculates a standard 16-bit TCP Checksum including the IP Pseudo-Header.
 * Expects network structural inputs to be provided in Network Byte Order (Big-Endian).
 */
uint16_t calculate_tcp_checksum(uint32_t src_ip, uint32_t dst_ip, 
                                void* tcp_packet_ptr, uint16_t tcp_total_len) 
{
    uint32_t sum = 0;
    
    // 1. Construct the 12-byte IP Pseudo-Header on the stack
    struct tcp_pseudo_header pseudo;
    pseudo.src_ip     = src_ip;
    pseudo.dst_ip     = dst_ip;
    pseudo.reserved   = 0;
    pseudo.protocol   = 6; // Protocol 6 = TCP
    pseudo.tcp_length = __builtin_bswap16(tcp_total_len); // Convert length to big-endian

    // 2. Accumulate the 16-bit words of the Pseudo-Header
    uint16_t* words = (uint16_t*)&pseudo;
    for (size_t i = 0; i < sizeof(struct tcp_pseudo_header) / 2; i++) {
        sum += words[i];
    }

    // 3. Accumulate the 16-bit words of the actual TCP Packet (Header + Data)
    words = (uint16_t*)tcp_packet_ptr;
    uint16_t length_left = tcp_total_len;

    while (length_left > 1) {
        sum += *words++;
        length_left -= 2;
    }

    // Handle trailing odd-byte padding block if the payload length is uneven
    if (length_left == 1) {
        sum += *(uint8_t*)words;
    }

    // 4. Fold the 32-bit accumulator back down into a strict 16-bit envelope space
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    // 5. Return bitwise one's complement result
    return (uint16_t)~sum;
}

/**
 * Packs a raw TCP frame header structure and appends upper-layer string payloads safely.
 */
void create_tcp_packet(struct tcp_packet* buf, void* payload, uint32_t len,
                       uint16_t src_port, uint16_t dst_port, 
                       uint32_t seq, uint32_t ack, uint8_t flags) 
{
    // Clear out any stale buffer memory first
    memset(buf, 0, sizeof(struct tcp_header) + len);

    // 1. Assign ports and connection tracking states in Network Byte Order (Big Endian)
    buf->hdr.src_port = __builtin_bswap16(src_port);
    buf->hdr.dst_port = __builtin_bswap16(dst_port);
    buf->hdr.seq_num  = __builtin_bswap32(seq);
    buf->hdr.ack_num  = __builtin_bswap32(ack);
    
    // 2. Set Header Length Data Offset. 
    // Standard TCP header without options is 20 bytes long. 
    // 20 bytes / 4 = 5 words. Shift left by 4 to populate the high nibble.
    buf->hdr.data_offset = (5 << 4);
    
    // 3. Bind Control Flags and Window capabilities
    buf->hdr.flags       = flags;
    buf->hdr.window_size = __builtin_bswap16(2048); // Announce 2048 bytes buffer allocation
    buf->hdr.urgent_ptr  = 0;
    buf->hdr.checksum    = 0; // Explicitly set to 0 before calculating checksum mathematically

    // 4. Append text or raw data payload behind the header boundaries if present
    if (payload && len > 0) {
        memcpy(buf->data, payload, len);
    }
}

/**
 * High-level helper to easily allocate, generate, calculate checksums for,
 * and dispatch an active TCP transaction packet straight over the IPv4 layer.
 */
void send_tcp_packet(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, 
                     uint32_t seq, uint32_t ack, uint8_t flags, 
                     void* payload, size_t payload_len) 
{
    // Calculate total continuous buffer footprint size
    size_t tcp_total_size = sizeof(struct tcp_header) + payload_len;
    
    struct tcp_packet* pkt = kmalloc(tcp_total_size);
    if (!pkt) return;

    // 1. Generate base structural headers
    create_tcp_packet(pkt, payload, payload_len, src_port, dst_port, seq, ack, flags);

    // 2. Perform the Pseudo-Header loop calculation using our local interface IP and target destination
    pkt->hdr.checksum = calculate_tcp_checksum(ip, dst_ip, pkt, tcp_total_size);

    // 3. Dispatch directly down over your network interface layer (Protocol 6 = TCP)
    send_ipv4_packet(dst_ip, (uint8_t*)pkt, 6, tcp_total_size);
    
    // 4. Free heap resource frame cleanly
    kfree(pkt);
}