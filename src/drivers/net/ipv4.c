#include <drivers/net/RTL8139.h>
#include <drivers/net/IPV4.h>
#include <string.h>
#include <stdint.h>

typedef uint32_t ipv4_addr_t;

// Global state tracking our own bound IP address
ipv4_addr_t ip = 0; 
static int lp = 0; // Packet Identification counter

/**
 * Standard 16-bit Internet Checksum implementation
 */
uint16_t ipv4_checksum(const void *data, size_t length)
{
    const uint16_t *words = data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += *words++;
        length -= 2;
    }

    if (length)
        sum += *(const uint8_t *)words;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

/**
 * Assigns/Binds the system's local IPv4 interface address (called by DHCP)
 */
void set_ip(uint32_t ips) {
    ip = ips;
}

/**
 * Encapsulates a network payload into an IPv4 and Ethernet frame,
 * handling automatic local gateway routing for external traffic.
 */
void send_ipv4_packet(uint32_t dest_ip, uint8_t* payload, int protocol, int len) {
    // 1. Calculate precise layout dimensions
    size_t ip_total_length = sizeof(struct ipv4_header) + len;
    size_t frame_total_size = sizeof(struct eth_frame) + ip_total_length;

    // 2. Allocate one single continuous buffer for the entire physical frame
    struct eth_frame *frame = kmalloc(frame_total_size);
    if (!frame) return; 

    // 3. Routing Layer Logic: Determine the proper Ethernet Destination MAC Address
    if (dest_ip == BROADCAST) {
        // Broadcast traffic (e.g., DHCP Discover/Request frames)
        memset(frame->dst, 0xFF, 6);
    } 
    else if ((dest_ip & 0xFFFFFF00) == (ip & 0xFFFFFF00)) {
        // Local Network traffic (Same Subnet /24)
        // Note: For a fully complete OS, an ARP request lookup would happen here.
        // For standard QEMU NAT testing, we can fall back to the gateway or broadcast.
        memset(frame->dst, 0xFF, 6); 
    } 
    else {
        // ROUTING TRICK: Traffic destined for external networks (like Google DNS 8.8.8.8)
        // must be encapsulated in an Ethernet frame addressing the QEMU Virtual Gateway Router.
        // QEMU's default network gateway MAC address is always 52:55:0A:00:02:02.
        frame->dst[0] = 0x52;
        frame->dst[1] = 0x55;
        frame->dst[2] = 0x0A;
        frame->dst[3] = 0x00;
        frame->dst[4] = 0x02;
        frame->dst[5] = 0x02;
    }

    // Populate remaining standard Ethernet framing fields
    memcpy(frame->src, get_mac(), 6);
    frame->ethertype = __builtin_bswap16(0x0800); // Big Endian for IPv4 Type

    // 4. Overlap a local pointer directly to the beginning of the Ethernet payload space
    struct ipv4_header *hdr = (struct ipv4_header*)frame->payload;
    
    // 5. Populate structural properties of the IPv4 Header fields
    hdr->version_ihl = 0x45;                      // IPv4, 20-byte Header length
    hdr->dscp_ecn = 0;                            // Default Quality of Service
    hdr->total_length = __builtin_bswap16(ip_total_length); // Total layout block size
    hdr->identification = __builtin_bswap16(lp++); // Unique sequence tracker
    hdr->flags_fragment = __builtin_bswap16(0x4000); // Don't Fragment flag set
    hdr->ttl = 64;                                // Time To Live (Hop count limit)
    hdr->protocol = protocol;                     // Protocol parameter (e.g., 17 for UDP)
    hdr->src_ip = ip;                             // Our bound local IP
    hdr->dst_ip = dest_ip;                        // Target outbound destination IP
    
    // Clear checksum field before calculating to avoid mathematical distortion
    hdr->checksum = 0;
    hdr->checksum = ipv4_checksum(hdr, sizeof(struct ipv4_header));

    // 6. Copy the upper-layer network payload (like a UDP packet) right behind the IP header
    memcpy(frame->payload + sizeof(struct ipv4_header), payload, len);

    // 7. Hand the finalized physical buffer frame down to the network hardware driver
    rtl8139_send(frame, frame_total_size);

    // 8. Free local heap memory allocations cleanly
    kfree(frame);
}