#include <drivers/net/IPV4.h>
#include <drivers/net/UDP.h>
#include <hals/net/RTL8139.h>
#include <drivers/fb.h>
#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)

// Standard 12-byte DNS Header
struct dns_header {
    uint16_t id;         // Identification/Transaction Number
    uint16_t flags;      // Flags (Query, Opcode, Recursion Desired, etc.)
    uint16_t qdcount;    // Number of Questions
    uint16_t ancount;    // Number of Answer resource records
    uint16_t nscount;    // Number of Authority records (ignored here)
    uint16_t arcount;    // Number of Additional records (ignored here)
};

// Fixed portion of a DNS Question entry
struct dns_question_tail {
    uint16_t qtype;      // Type of query (e.g., 0x0001 for A record)
    uint16_t qclass;     // Class of query (e.g., 0x0001 for Internet IN)
};

// Fixed portion of a DNS Answer Resource Record (RR)
struct dns_answer_header {
    uint16_t type;       // Type (0x0001 = A)
    uint16_t class;      // Class (0x0001 = IN)
    uint32_t ttl;        // Time to live
    uint16_t rdlength;   // Length of resource data (4 bytes for IPv4)
};

#pragma pack(pop)

/**
 * Converts a standard string like "gemini.google.com" into the DNS format:
 * \006gemini\006google\003com\000
 * Allocates buffer memory internally; caller must kfree the returned pointer.
 */
static uint8_t* format_dns_name(const char* hostname, size_t* out_len) {
    size_t len = strlen(hostname);
    // DNS formatting adds 1 extra byte for the first label size, and 1 byte for the trailing 0
    size_t dest_len = len + 2; 
    
    uint8_t* dest = kmalloc(dest_len);
    if (!dest) return NULL;

    size_t label_start = 0;
    size_t dest_idx = 1; // Leave slot 0 for the first chunk length

    for (size_t i = 0; i <= len; i++) {
        if (hostname[i] == '.' || hostname[i] == '\0') {
            size_t chunk_len = i - label_start;
            dest[label_start] = (uint8_t)chunk_len; // Set chunk size back on its respective length index slot
            
            if (hostname[i] == '\0') {
                dest[dest_idx++] = 0; // Final terminating null label
                break;
            }
            
            label_start = dest_idx;
            dest_idx++;
        } else {
            dest[dest_idx++] = hostname[i];
        }
    }

    *out_len = dest_len;
    return dest;
}

/**
 * Skips a variable-length DNS string name fields inside a response payload.
 * DNS names use compression pointers (0xC0XX). This helper skips either the full string
 * or the 2-byte compressed marker to safely advance the parsing index.
 */
static uint8_t* skip_dns_name(uint8_t* current_ptr, uint8_t* dns_start) {
    while (*current_ptr != 0) {
        if ((*current_ptr & 0xC0) == 0xC0) {
            // Compressed pointer. It takes 2 bytes total. Skip and return.
            return current_ptr + 2;
        }
        // Normal label chunk: skip over length indicator byte + string characters
        current_ptr += *current_ptr + 1;
    }
    return current_ptr + 1; // Skip the terminating zero byte
}

/**
 * Queries Google DNS (8.8.8.8) to resolve an IPv4 address for a hostname.
 * Returns the IPv4 Address (Big Endian) or 0 on failure.
 */
uint32_t dns_lookup(const char* hostname) {
    size_t name_len = 0;
    uint8_t* dns_name = format_dns_name(hostname, &name_len);
    if (!dns_name) return 0;

    // 1. Calculate DNS layer sizing and build frame structures
    size_t dns_layer_size = sizeof(struct dns_header) + name_len + sizeof(struct dns_question_tail);
    uint8_t* dns_payload = kmalloc(dns_layer_size);
    if (!dns_payload) {
        kfree(dns_name);
        return 0;
    }

    struct dns_header* dns_hdr = (struct dns_header*)dns_payload;
    dns_hdr->id = __builtin_bswap16(0x55AA);      // Arbitrary Unique Transaction ID
    dns_hdr->flags = __builtin_bswap16(0x0100);   // Standard query with recursion desired
    dns_hdr->qdcount = __builtin_bswap16(1);      // 1 Question
    dns_hdr->ancount = 0; dns_hdr->nscount = 0; dns_hdr->arcount = 0;

    // Copy formatted name string into payload
    memcpy(dns_payload + sizeof(struct dns_header), dns_name, name_len);
    kfree(dns_name); // Free temp name buffer

    // Append the question type and class configuration right at the tail offset
    struct dns_question_tail* tail = (struct dns_question_tail*)(dns_payload + sizeof(struct dns_header) + name_len);
    tail->qtype = __builtin_bswap16(1);  // Type A (IPv4 Host Address)
    tail->qclass = __builtin_bswap16(1); // Class IN (Internet)

    // 2. Encapsulate into UDP Envelope (Source Port: 51234 -> Dest Port: 53)
    size_t udp_total_size = sizeof(struct udp_header) + dns_layer_size;
    struct udp_packet* upkt = kmalloc(udp_total_size);
    if (!upkt) {
        kfree(dns_payload);
        return 0;
    }

    create_udp_packet(upkt, dns_payload, dns_layer_size, 51234, 53);
    kfree(dns_payload);

    // 3. Dispatch to Google DNS via our rewritten IPv4 Layer
    uint32_t google_dns_ip = 0x08080808; // 8.8.8.8
    printk(LOG_TRACE, "[DNS] Querying Google DNS for: %s\n", hostname);
    send_ipv4_packet(google_dns_ip, (uint8_t*)upkt, 17, udp_total_size);
    kfree(upkt);

    // 4. Listen and poll for incoming UDP response packet
    uint8_t* poll_buf = kmalloc(2048);
    if (!poll_buf) return 0;

    uint32_t resolved_ip = 0;
    uint32_t timeout_loops = 0;

    while (timeout_loops++ < 4000000) {
        memset(poll_buf, 0, 2048);
        rtl8139_poll(poll_buf);

        // Frame filtering: IPv4 -> UDP -> Port 53
        if (((poll_buf[12] << 8) | poll_buf[13]) != 0x0800) continue; 
        if (poll_buf[23] != 17) continue;                            
        if (((poll_buf[34] << 8) | poll_buf[35]) != 53) continue;    

        // Marker pointing to the beginning of the DNS data block
        uint8_t* dns_start = &poll_buf[42];
        struct dns_header* reply_hdr = (struct dns_header*)dns_start;

        // Verify transaction match and ensure it's marked as a response reply
        if (reply_hdr->id == __builtin_bswap16(0x55AA)) {
            uint16_t answers = __builtin_bswap16(reply_hdr->ancount);
            if (answers == 0) {
                printk(LOG_TRACE, "[DNS] Error: Hostname could not be found or resolved.\n");
                break;
            }

            // 5. Parse through the message payload to locate the answer records.
            // Move cursor past the 12-byte header
            uint8_t* reader = dns_start + sizeof(struct dns_header);

            // Skip past the Echoed Question part that the server sends back
            reader = skip_dns_name(reader, dns_start);
            reader += sizeof(struct dns_question_tail); // Step over QTYPE and QCLASS

            // We are now at the start of the Answer Records Array!
            for (int i = 0; i < answers; i++) {
                reader = skip_dns_name(reader, dns_start); // Skip owner name field
                struct dns_answer_header* ans = (struct dns_answer_header*)reader;
                reader += sizeof(struct dns_answer_header); // Skip to response data payload

                uint16_t type = __builtin_bswap16(ans->type);
                uint16_t rdlen = __builtin_bswap16(ans->rdlength);

                if (type == 1 && rdlen == 4) { // Valid A Record with IPv4 address payload size
                    // Copy out the raw 4-byte IP address
                    memcpy(&resolved_ip, reader, 4);
                    break;
                }
                
                // If it's a CNAME alias instead of an A record, skip its payload data and continue looping
                reader += rdlen;
            }
            break;
        }
    }

    kfree(poll_buf);
    return resolved_ip; 
}
/**
 * Helper to log a uint32_t IP address in standard dot-decimal notation (X.X.X.X).
 * Handles Network Byte Order (Big-Endian) data layout.
 */
void log_ip(uint32_t ip_addr) {
    // Cast the uint32_t pointer to a uint8_t pointer to access the bytes sequentially
    uint8_t *bytes = (uint8_t *)&ip_addr;

    // Print the 4 individual octets separated by dots
    printk(LOG_TRACE, "%d.%d.%d.%d\n", bytes[0], bytes[1], bytes[2], bytes[3]);
}