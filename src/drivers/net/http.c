#include <drivers/net/IPV4.h>
#include <drivers/net/TCP.h>
#include <drivers/net/RTL8139.h>
#include <drivers/fb.h>
#include <stdint.h>
#include <string.h>

/**
 * Helper to translate a base-10 positive integer to string format.
 * Avoids any dependency on snprintk or sprintf.
 */
static int local_itoa(int value, char* result_str) {
    char temp[12];
    int i = 0;
    int len = 0;

    if (value == 0) {
        result_str[0] = '0';
        result_str[1] = '\0';
        return 1;
    }

    while (value > 0) {
        temp[i++] = (value % 10) + '0';
        value /= 10;
    }

    len = i;
    for (int j = 0; j < len; j++) {
        result_str[j] = temp[--i];
    }
    result_str[len] = '\0';
    return len;
}

/**
 * Establishes a raw TCP stream connection, executes a 3-way handshake,
 * dispatches an HTTP GET request, and populates the resulting payload into out_buf.
 * Returns the total number of bytes written to the buffer, or 0 on failure.
 */
uint32_t http_get_buffer(const char* hostname, const char* path, char* out_buf, uint32_t max_len) {
    uint32_t server_ip = dns_lookup(hostname);
    if (server_ip == 0) {
        printk(LOG_TRACE, "[HTTP] DNS Resolution failed for target host\n");
        return 0;
    }

    uint16_t local_port = 53100;     // Choose a high local ephemeral client port
    uint32_t my_seq = 0x55AA1122;    // Pick an initial sequence tracker number
    uint32_t server_seq = 0;
    uint32_t bytes_written = 0;

    if (out_buf && max_len > 0) {
        out_buf[0] = '\0';
    } else {
        return 0;
    }

    // ========================================================
    // HANDSHAKE STEP 1: Transmit Outbound TCP SYN
    // ========================================================
    printk(LOG_TRACE, "[HTTP] Dispatching connection attempt (SYN) to target web server...\n");
    send_tcp_packet(server_ip, local_port, 80, my_seq, 0, TCP_FLAG_SYN, NULL, 0);

    uint8_t* poll_buf = kmalloc(2048);
    if (!poll_buf) return 0;

    uint32_t timeout = 0;
    int connected = 0;

    // ========================================================
    // HANDSHAKE STEP 2: Listen for incoming Server SYN-ACK
    // ========================================================
    while (timeout++ < 20000000) {
        memset(poll_buf, 0, 2048);
        rtl8139_poll(poll_buf);

        if (((poll_buf[12] << 8) | poll_buf[13]) != 0x0800) continue;
        if (poll_buf[23] != 6) continue;

        struct tcp_header* reply = (struct tcp_header*)&poll_buf[34];

        if (__builtin_bswap16(reply->dst_port) == local_port) {
            if (reply->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                server_seq = __builtin_bswap32(reply->seq_num);
                my_seq = __builtin_bswap32(reply->ack_num); 
                
                printk(LOG_TRACE, "[HTTP] SYN-ACK received! Acknowledging sequence connection...\n");
                connected = 1;
                break;
            }
        }
    }

    if (!connected) {
        printk(LOG_TRACE, "[HTTP] Handshake request failed or timed out.\n");
        kfree(poll_buf);
        return 0;
    }

    // ========================================================
    // STEP 3: Dispatch HTTP GET request headers manually
    // ========================================================
    printk(LOG_TRACE, "[HTTP] Sending GET payload request context blocks...\n");

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "GET ", 4);
    my_seq += 4;
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, (void*)path, strlen(path));
    my_seq += strlen(path);
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, " HTTP/1.1\r\n", 11);
    my_seq += 11;

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "Host: ", 6);
    my_seq += 6;
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, (void*)hostname, strlen(hostname));
    my_seq += strlen(hostname);
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "\r\n", 2);
    my_seq += 2;

    char* final_headers = "User-Agent: HobbyOS-Kernel\r\nConnection: close\r\n\r\n";
    size_t final_len = strlen(final_headers);
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK | TCP_FLAG_PSH, final_headers, final_len);
    my_seq += final_len;

    // ========================================================
    // STEP 4: Enter data polling stream collection loop
    // ========================================================
    printk(LOG_TRACE, "[HTTP] Collecting data stream into destination memory buffer...\n");
    timeout = 0;

    while (timeout++ < 40000000) { // Multiplied upper boundary scale limit
        while (timeout++ < 40000000) {
            memset(poll_buf, 0, 2048);
            rtl8139_poll(poll_buf);

            if (((poll_buf[12] << 8) | poll_buf[13]) != 0x0800) continue;
            if (poll_buf[23] != 6) continue;

            struct tcp_header* reply = (struct tcp_header*)&poll_buf[34];

            if (__builtin_bswap16(reply->dst_port) == local_port) {
                // Reset timeout counter whenever we receive any traffic belonging to this connection
                timeout = 0;

                size_t ip_hdr_len = (poll_buf[14] & 0x0F) * 4;
                size_t tcp_hdr_len = ((reply->data_offset >> 4) & 0x0F) * 4;
                
                uint16_t ip_total_len = (poll_buf[16] << 8) | poll_buf[17];
                size_t tcp_payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
                printk(LOG_TRACE, "IP total=%u\n", ip_total_len);
                printk(LOG_TRACE, "IP hdr=%u\n", ip_hdr_len);
                printk(LOG_TRACE, "TCP hdr=%u\n", tcp_hdr_len);
                printk(LOG_TRACE, "Payload=%u\n", tcp_payload_len);

                if (tcp_payload_len > 0) {
                    uint8_t* data_stream = (uint8_t*)reply + tcp_hdr_len;

                    uint32_t space_left = max_len - bytes_written - 1; 
                    if (space_left > 0) {
                        uint32_t chunk_size = (tcp_payload_len > space_left) ? space_left : tcp_payload_len;
                        
                        memcpy(out_buf + bytes_written, data_stream, chunk_size);
                        bytes_written += chunk_size;
                        out_buf[bytes_written] = '\0';
                    }

                    // Explicitly ACK the received sequence block bytes to keep server streaming
                    server_seq = __builtin_bswap32(reply->seq_num) + tcp_payload_len;
                    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq, TCP_FLAG_ACK, NULL, 0);
                }

                if (reply->flags & TCP_FLAG_FIN) {
                    printk(LOG_TRACE, "[HTTP] Content collected successfully. Connection finished by server.\n");
                    
                    // Acknowledge the payload size AND the 1 sync byte consumed by the server FIN flag
                    server_seq = __builtin_bswap32(reply->seq_num) + tcp_payload_len + 1;
                    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq, TCP_FLAG_ACK, NULL, 0);

                    kfree(poll_buf);
                    return bytes_written;
                }
            }
        }
    }

    printk(LOG_TRACE, "[HTTP] Warning: Stream listener exited via timeout. Collected %d bytes.\n", bytes_written);
    kfree(poll_buf);
    return bytes_written;
}

/**
 * Establishes a raw TCP stream connection, executes a 3-way handshake,
 * dispatches an HTTP POST request carrying a data body payload, 
 * and populates the resulting response stream into out_buf.
 * Returns the total number of response bytes written to out_buf, or 0 on failure.
 */
uint32_t http_post_buffer(const char* hostname, const char* path, const char* post_data, char* out_buf, uint32_t max_len) {
    uint32_t server_ip = dns_lookup(hostname);
    if (server_ip == 0) {
        printk(LOG_TRACE, "[HTTP] DNS Resolution failed for target host\n");
        return 0;
    }

    uint16_t local_port = 53200;     // Choose an ephemeral client port
    uint32_t my_seq = 0x55AA3344;    // Pick an initial sequence tracker number
    uint32_t server_seq = 0;
    uint32_t bytes_written = 0;
    size_t post_data_len = strlen(post_data);

    if (out_buf && max_len > 0) {
        out_buf[0] = '\0';
    } else {
        return 0;
    }

    // ========================================================
    // HANDSHAKE STEP 1: Transmit Outbound TCP SYN
    // ========================================================
    printk(LOG_TRACE, "[HTTP] Dispatching POST connection attempt (SYN)...\n");
    send_tcp_packet(server_ip, local_port, 80, my_seq, 0, TCP_FLAG_SYN, NULL, 0);

    uint8_t* poll_buf = kmalloc(2048);
    if (!poll_buf) return 0;

    uint32_t timeout = 0;
    int connected = 0;

    // ========================================================
    // HANDSHAKE STEP 2: Listen for incoming Server SYN-ACK
    // ========================================================
    while (timeout++ < 20000000) {
        memset(poll_buf, 0, 2048);
        rtl8139_poll(poll_buf);

        if (((poll_buf[12] << 8) | poll_buf[13]) != 0x0800) continue;
        if (poll_buf[23] != 6) continue;

        struct tcp_header* reply = (struct tcp_header*)&poll_buf[34];

        if (__builtin_bswap16(reply->dst_port) == local_port) {
            if (reply->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                server_seq = __builtin_bswap32(reply->seq_num);
                my_seq = __builtin_bswap32(reply->ack_num);
                
                printk(LOG_TRACE, "[HTTP] SYN-ACK received! Acknowledging POST track stream...\n");
                connected = 1;
                break;
            }
        }
    }

    if (!connected) {
        printk(LOG_TRACE, "[HTTP] Handshake request failed or timed out.\n");
        kfree(poll_buf);
        return 0;
    }

    // ========================================================
    // STEP 3: Dispatch HTTP POST Headers and Body Payload
    // ========================================================
    printk(LOG_TRACE, "[HTTP] Dispatching POST headers & request body data chunks...\n");

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "POST ", 5);
    my_seq += 5;
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, (void*)path, strlen(path));
    my_seq += strlen(path);
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, " HTTP/1.1\r\n", 11);
    my_seq += 11;

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "Host: ", 6);
    my_seq += 6;
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, (void*)hostname, strlen(hostname));
    my_seq += strlen(hostname);
    
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "\r\n", 2);
    my_seq += 2;

    char* content_type = "Content-Type: text/plain\r\n";
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, content_type, strlen(content_type));
    my_seq += strlen(content_type);

    char length_str[12];
    int length_str_len = local_itoa(post_data_len, length_str);

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "Content-Length: ", 16);
    my_seq += 16;

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, length_str, length_str_len);
    my_seq += length_str_len;

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, "\r\n", 2);
    my_seq += 2;

    char* final_headers = "User-Agent: HobbyOS-Kernel\r\nConnection: close\r\n\r\n";
    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK, final_headers, strlen(final_headers));
    my_seq += strlen(final_headers);

    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq + 1, TCP_FLAG_ACK | TCP_FLAG_PSH, (void*)post_data, post_data_len);
    my_seq += post_data_len;

    // ========================================================
    // STEP 4: Enter data polling stream collection loop
    // ========================================================
    printk(LOG_TRACE, "[HTTP] Gathering server response body metrics...\n");
    timeout = 0;

    while (timeout++ < 40000000) {
        while (rtl8139_has_packet()) {
            memset(poll_buf, 0, 2048);
            rtl8139_poll(poll_buf);

            if (((poll_buf[12] << 8) | poll_buf[13]) != 0x0800) continue;
            if (poll_buf[23] != 6) continue;

            struct tcp_header* reply = (struct tcp_header*)&poll_buf[34];

            if (__builtin_bswap16(reply->dst_port) == local_port) {
                // Reset timeout counter on network activity
                timeout = 0;

                size_t ip_hdr_len = (poll_buf[14] & 0x0F) * 4;
                size_t tcp_hdr_len = ((reply->data_offset >> 4) & 0x0F) * 4;
                
                uint16_t ip_total_len = (poll_buf[16] << 8) | poll_buf[17];
                size_t tcp_payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
                

                if (tcp_payload_len > 0) {
                    uint8_t* data_stream = (uint8_t*)reply + tcp_hdr_len;

                    uint32_t space_left = max_len - bytes_written - 1; 
                    if (space_left > 0) {
                        uint32_t chunk_size = (tcp_payload_len > space_left) ? space_left : tcp_payload_len;
                        
                        memcpy(out_buf + bytes_written, data_stream, chunk_size);
                        bytes_written += chunk_size;
                        out_buf[bytes_written] = '\0';
                    }

                    // Explicitly ACK the received sequence block bytes to keep server streaming
                    server_seq = __builtin_bswap32(reply->seq_num) + tcp_payload_len;
                    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq, TCP_FLAG_ACK, NULL, 0);
                }

                if (reply->flags & TCP_FLAG_FIN) {
                    printk(LOG_TRACE, "[HTTP] POST Response collected completely.\n");
                    
                    // Acknowledge the payload size AND the 1 sync byte consumed by the server FIN flag
                    server_seq = __builtin_bswap32(reply->seq_num) + tcp_payload_len + 1;
                    send_tcp_packet(server_ip, local_port, 80, my_seq, server_seq, TCP_FLAG_ACK, NULL, 0);

                    kfree(poll_buf);
                    return bytes_written;
                }
            }
        }
    }
    
    kfree(poll_buf);
    return bytes_written;
}