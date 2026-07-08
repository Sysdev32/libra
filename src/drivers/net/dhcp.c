#include <drivers/net/IPV4.h>
#include <drivers/net/UDP.h>
#include <drivers/net/RTL8139.h>
#include <drivers/pci.h>
#include <drivers/net/HTTP.h>
#include <drivers/fb.h>
#include <stdint.h>
#include <string.h>

#define DHCP_CHADDR_LEN  16
#define DHCP_SNAME_LEN   64
#define DHCP_FILE_LEN    128
#define DHCP_OPTIONS_LEN 308 

#pragma pack(push, 1)

struct dhcp_packet {
    uint8_t  op;                
    uint8_t  htype;             
    uint8_t  hlen;              
    uint8_t  hops;              
    uint32_t xid;               
    uint16_t secs;              
    uint16_t flags;             
    uint32_t ciaddr;            
    uint32_t yiaddr;            
    uint32_t siaddr;            
    uint32_t giaddr;            
    uint8_t  chaddr[DHCP_CHADDR_LEN]; 
    char     sname[DHCP_SNAME_LEN];   
    char     file[DHCP_FILE_LEN];    
    uint8_t  options[DHCP_OPTIONS_LEN]; 
};

#pragma pack(pop)

// Helper to safely get random transaction IDs
uint32_t get_hardware_random_x86(void) {
    static uint64_t seed = 0x123456789ABCDEFULL; 
    seed = (6364136223846793005ULL * seed) + 1442695040888963407ULL;
    return (uint32_t)(seed >> 32);
}

/**
 * Builds a base DHCP structure with default hardware parameters
 */
static void init_base_dhcp_packet(struct dhcp_packet *pkt, uint32_t xid) {
    memset(pkt, 0, sizeof(struct dhcp_packet));
    pkt->op = 1;                              // BOOTREQUEST
    pkt->htype = 1;                           // Ethernet
    pkt->hlen = 6;                            // MAC address length
    pkt->xid = xid;                           // Transaction ID
    pkt->flags = __builtin_bswap16(0x8000);   // Force Broadcast Reply
    
    memcpy(pkt->chaddr, get_mac(), 6);        // Our MAC address

    // Magic Cookie
    pkt->options[0] = 0x63; pkt->options[1] = 0x82; pkt->options[2] = 0x53; pkt->options[3] = 0x63;
}

/**
 * Populates options specifically for a DHCP DISCOVER packet
 */
void generate_dhcp_discover(struct dhcp_packet *pkt, uint32_t xid) {
    init_base_dhcp_packet(pkt, xid);

    // Option 53: DHCP Message Type (Length 1, Value 1 = Discover)
    pkt->options[4] = 53;  pkt->options[5] = 1; pkt->options[6] = 1;   
    
    // Option 55: Parameter Request List (Subnet Mask, Router, DNS)
    pkt->options[7] = 53;  pkt->options[8] = 3; pkt->options[9] = 1; pkt->options[10] = 3; pkt->options[11] = 6;   
    
    pkt->options[12] = 0xFF; // End Option
}

/**
 * Populates options specifically for a DHCP REQUEST packet to accept an offer
 */
void generate_dhcp_request(struct dhcp_packet *pkt, uint32_t xid, uint32_t requested_ip, uint32_t server_ip) {
    init_base_dhcp_packet(pkt, xid);

    // Option 53: DHCP Message Type (Length 1, Value 3 = Request)
    pkt->options[4] = 53; pkt->options[5] = 1; pkt->options[6] = 3;

    // Option 50: Requested IP Address (Length 4)
    pkt->options[7] = 50; pkt->options[8] = 4;
    memcpy(&pkt->options[9], &requested_ip, 4);

    // Option 54: Server Identifier (Length 4)
    pkt->options[13] = 54; pkt->options[14] = 4;
    memcpy(&pkt->options[15], &server_ip, 4);

    pkt->options[19] = 0xFF; // End Option
}

/**
 * Helper to wrap a DHCP payload inside UDP/IPv4 envelopes and transmit over RTL8139
 */
static void broadcast_dhcp_packet(struct dhcp_packet *pkt) {
    size_t dhcp_size = sizeof(struct dhcp_packet);
    size_t udp_total_size = sizeof(struct udp_header) + dhcp_size;

    struct udp_packet *upkt = kmalloc(udp_total_size);
    if (!upkt) return;

    create_udp_packet(upkt, pkt, dhcp_size, 68, 67);
    send_ipv4_packet(BROADCAST, (uint8_t*)upkt, 17, udp_total_size);

    kfree(upkt);
}

/**
 * High-level rewritten entry point for running the full DHCP state transaction
 */
uint32_t discover() {
    uint32_t transaction_id = get_hardware_random_x86();
    uint32_t offered_ip = 0;
    uint32_t server_ip = 0;
    
    struct dhcp_packet *pkt = kmalloc(sizeof(struct dhcp_packet));
    uint8_t *poll_buf = kmalloc(2048);
    
    if (!pkt || !poll_buf) {
        if (pkt) kfree(pkt);
        if (poll_buf) kfree(poll_buf);
        return 0;
    }

    // ==========================================
    // STATE 1: SEND DISCOVER & WAIT FOR OFFER
    // ==========================================
    generate_dhcp_discover(pkt, transaction_id);
    broadcast_dhcp_packet(pkt);
    printk(LOG_TRACE, "[DHCP] Discover broadcasted. Polling for Offer...\n");

    uint32_t timeout = 0;
    while (timeout++ < 2000000) {
        memset(poll_buf, 0, 2048);
        rtl8139_poll(poll_buf);

        // Validate basic layer-2 and layer-3 configurations
        if (((poll_buf[12] << 8) | poll_buf[13]) != 0x0800) continue; // Not IPv4
        if (poll_buf[23] != 17) continue;                            // Not UDP
        if (((poll_buf[34] << 8) | poll_buf[35]) != 67) continue;    // Not Port 67

        struct dhcp_packet* reply = (struct dhcp_packet*)&poll_buf[42];
        if (reply->op == 2 && reply->xid == transaction_id) { // BOOTREPLY
            
            // Extract Option 53 to verify it's an Offer (Value 2)
            if (reply->options[4] == 53 && reply->options[6] == 2) {
                offered_ip = reply->yiaddr;
                server_ip = reply->siaddr;  // Capture Server Next-Hop
                
                // Backup fall-back: Parse Option 54 for Server ID if siaddr is empty
                if (server_ip == 0 && reply->options[13] == 54) {
                    memcpy(&server_ip, &reply->options[15], 4);
                }

                printk(LOG_TRACE, "[DHCP] Offer received: Offered IP: 0x%08X from Server: 0x%08X\n", offered_ip, server_ip);
                break;
            }
        }
    }

    if (offered_ip == 0) {
        printk(LOG_TRACE, "[DHCP] Error: State timeout out waiting for Offer.\n");
        kfree(poll_buf);
        kfree(pkt);
        return 0;
    }

    // ==========================================
    // STATE 2: SEND REQUEST & WAIT FOR ACK
    // ==========================================
    transaction_id++; // Advance transaction identification sequence
    generate_dhcp_request(pkt, transaction_id, offered_ip, server_ip);
    broadcast_dhcp_packet(pkt);
    printk(LOG_TRACE, "[DHCP] Request broadcasted. Polling for final ACK...\n");

    uint32_t final_ip = 0;
    timeout = 0;
    
    while (timeout++ < 2000000) {
        memset(poll_buf, 0, 2048);
        rtl8139_poll(poll_buf);

        if (((poll_buf[12] << 8) | poll_buf[13]) != 0x0800) continue; 
        if (poll_buf[23] != 17) continue;                            
        if (((poll_buf[34] << 8) | poll_buf[35]) != 67) continue;    

        struct dhcp_packet* reply = (struct dhcp_packet*)&poll_buf[42];
        if (reply->op == 2 && reply->xid == transaction_id) {
            
            // Check Option 53 for DHCP ACK (Value 5)
            if (reply->options[4] == 53 && reply->options[6] == 5) {
                final_ip = reply->yiaddr;
                printk(LOG_TRACE, "[DHCP] ACK Received! Lease successfully finalized.\n");
                break;
            } 
            // Check Option 53 for DHCP NAK (Value 6)
            else if (reply->options[4] == 53 && reply->options[6] == 6) {
                printk(LOG_TRACE, "[DHCP] NAK Received! Server rejected lease assignment request.\n");
                break;
            }
        }
    }

    // Apply configuration setting to the IPv4 Layer state if successful
    if (final_ip != 0) {
        set_ip(final_ip);
        printk(LOG_TRACE, "[DHCP] Client bound completely. Interface Active IP assigned.\n");
    } else {
        printk(LOG_TRACE, "[DHCP] Error: Transaction failed to reach ACK lease lock phase.\n");
    }

    // 4. Memory sweeps and hardware tracking cleanup
    kfree(poll_buf);
    kfree(pkt);
    return final_ip;
}