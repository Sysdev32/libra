#include <stdint.h>
#include <stddef.h>
#include <hals/net/RTL8139.h>
#include <drivers/net/HTTP.h>
#include <drivers/net/IPV4.h>
#include <drivers/net/TCP.h>
#include <drivers/net/UDP.h>
#include <drivers/net/nsock.h>
int udp_recv(struct net_socket* socket, void* buffer, size_t length) {
    void* poll = kmalloc(1024 * 4);
    uint16_t len = 0;
    while (1) {
        rtl8139_poll(poll);  
        struct eth_frame *frame = (struct eth_frame*)poll;
        if (ntohs(frame->ethertype) == 0x0800) {
            struct ipv4_packet *ipv4 = (struct ipv4_packet*)frame->payload;
            if (ipv4->hdr.protocol == 17) {
                struct udp_packet *udp = (struct udp_packet*)ipv4->payload;
                len = udp->hdr.length;
                if (len > length) {
                    len = length;
                }
                memcpy(buffer, udp->data, len);
                break;
            }
        }
    } 
    kfree(poll);
    return len;
}

int is_ipv4(const char *s) {
    int dots = 0;

    while (*s) {
        if (*s == '.') {
            dots++;
        } else if (*s < '0' || *s > '9') {
            return 0;
        }
        s++;
    }

    return dots == 3;
}
int net_parse_url(const char *url, net_url_t *out)
{
    if (!url || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    const char *p = url;

    /* Parse scheme */
    while (*p && *p != ':' && *p != '/')
        p++;

    if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
        memcpy(out->scheme, url, p - url);
        out->scheme[p - url] = '\0';
        url = p + 3;
    } else {
        memcpy(out->scheme, "http", 5);
    }

    /* Parse host */
    p = url;
    while (*p && *p != ':' && *p != '/')
        p++;

    memcpy(out->host, url, p - url);
    out->host[p - url] = '\0';

    url = p;

    /* Parse port */
    if (*url == ':') {
        url++;

        out->port = 0;
        while (*url >= '0' && *url <= '9') {
            out->port = out->port * 10 + (*url - '0');
            url++;
        }
    } else {
        if (!strcmp(out->scheme, "https"))
            out->port = 443;
        else
            out->port = 80;
    }

    /* Parse path */
    if (*url == '/') {
        p = url;
        while (*p)
            p++;

        memcpy(out->path, url, p - url);
        out->path[p - url] = '\0';
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    return 0;
}

int udp_send(struct net_socket* socket, const void* buffer, size_t length) {
    if (!socket->md.connected) return -1;
    size_t pkt_len = sizeof(struct udp_packet) + length;

    struct udp_packet *pkt = kmalloc(pkt_len);

    create_udp_packet(pkt, buffer, length,
                    socket->md.my_udp_port,
                    socket->md.connect_udp_port);

    send_ipv4_packet(socket->md.dest_ip, pkt, 17, pkt_len);
    kfree(pkt);
    return length;
}
int ipv4_parse(const char *str, uint32_t *out)
{
    uint32_t ip = 0;
    uint32_t octet = 0;
    int dots = 0;

    while (*str) {
        if (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (*str - '0');
            if (octet > 255)
                return -1;
        } else if (*str == '.') {
            if (dots >= 3)
                return -1;

            ip = (ip << 8) | octet;
            octet = 0;
            dots++;
        } else {
            return -1;
        }

        str++;
    }

    if (dots != 3)
        return -1;

    ip = (ip << 8) | octet;

    *out = ip; // Stored in network byte order (big-endian)
    return 0;
}
int last_port = 41952;
int last_socket = -1;
int udp_connect(struct net_socket* socket, const char* addr) {
    net_url_t url;
    net_parse_url(addr, &url);
    socket->md.connect_udp_port = url.port;
    socket->md.my_udp_port = last_port++;
    socket->md.connected = 1;
    if (is_ipv4(url.host)) {
        ipv4_parse(url.path, &socket->md.dest_ip);
    } else {
        socket->md.dest_ip = dns_lookup(url.path);
    }
    strcpy(socket->md.path, url.path);
    return 0;
}
int udp_close(struct net_socket* socket) {
    // do nothing for now lol
}
// ==========================================
// RAW PROTOCOL HANDLERS
// ==========================================

int raw_recv(struct net_socket* socket, void* buffer, size_t length) {
    void* poll = kmalloc(1024 * 4);
    uint16_t len = 0;
    while (1) {
        rtl8139_poll(poll);
        struct eth_frame *frame = (struct eth_frame*)poll;
        // In a raw socket, we grab the raw payload of the ethernet frame
        if (ntohs(frame->ethertype) == 0x0800) {
            // Adjust copy limits based on max buffer length
            len = (1024 * 4) - sizeof(struct eth_frame);
            if (len > length) {
                len = length;
            }
            memcpy(buffer, frame->payload, len);
            break;
        }
    }
    kfree(poll);
    return len;
}

int raw_send(struct net_socket* socket, const void* buffer, size_t length) {
    if (!socket->md.connected) return -1;
    // For raw networking, we route the raw buffer through IPv4 directly 
    // using a dummy protocol or a protocol stored in metadata if applicable.
    send_ipv4_packet(socket->md.dest_ip, (uint8_t*)buffer, 255, length); 
    return length;
}

int raw_connect(struct net_socket* socket, const char* addr) {
    net_url_t url;
    net_parse_url(addr, &url);
    socket->md.connected = 1;
    if (is_ipv4(url.host)) {
        ipv4_parse(url.host, &socket->md.dest_ip);
    } else {
        socket->md.dest_ip = dns_lookup(url.host);
    }
    strcpy(socket->md.path, url.path);
    return 0;
}

int raw_close(struct net_socket* socket) {
    socket->md.connected = 0;
    return 0;
}

// ==========================================
// TCP PROTOCOL HANDLERS
// ==========================================

int tcp_connect(struct net_socket* socket, const char* addr) {
    net_url_t url;
    net_parse_url(addr, &url);
    
    socket->md.connect_tcp_port = url.port;
    socket->md.my_tcp_port = last_port++;
    
    if (is_ipv4(url.host)) {
        ipv4_parse(url.host, &socket->md.dest_ip);
    } else {
        socket->md.dest_ip = dns_lookup(url.host);
    }
    strcpy(socket->md.path, url.path);

    // 1. Send SYN packet to establish connection
    size_t pkt_len = sizeof(struct tcp_packet);
    struct tcp_packet *syn_pkt = kmalloc(pkt_len);
    
    create_tcp_packet(syn_pkt, NULL, 0, 
                      socket->md.my_tcp_port, 
                      socket->md.connect_tcp_port, 
                      100, 0, TCP_FLAG_SYN);
                      
    send_ipv4_packet(socket->md.dest_ip, (uint8_t*)syn_pkt, 6, pkt_len);
    kfree(syn_pkt);

    // 2. Poll for SYN-ACK response
    void* poll = kmalloc(1024 * 4);
    while (1) {
        rtl8139_poll(poll);
        struct eth_frame *frame = (struct eth_frame*)poll;
        if (ntohs(frame->ethertype) == 0x0800) {
            struct ipv4_packet *ipv4 = (struct ipv4_packet*)frame->payload;
            if (ipv4->hdr.protocol == 6) { // TCP Protocol number is 6
                struct tcp_packet *tcp = (struct tcp_packet*)ipv4->payload;
                if ((tcp->hdr.flags & TCP_FLAG_SYN) && (tcp->hdr.flags & TCP_FLAG_ACK)) {
                    // Properly matched remote server reply
                    break;
                }
            }
        }
    }
    kfree(poll);

    // 3. Complete Handshake by sending ACK
    struct tcp_packet *ack_pkt = kmalloc(pkt_len);
    create_tcp_packet(ack_pkt, NULL, 0, 
                      socket->md.my_tcp_port, 
                      socket->md.connect_tcp_port, 
                      101, 101, TCP_FLAG_ACK);
                      
    send_ipv4_packet(socket->md.dest_ip, (uint8_t*)ack_pkt, 6, pkt_len);
    kfree(ack_pkt);

    socket->md.connected = 1;
    return 0;
}

int tcp_send(struct net_socket* socket, const void* buffer, size_t length) {
    if (!socket->md.connected) return -1;

    size_t pkt_len = sizeof(struct tcp_packet) + length;
    struct tcp_packet *pkt = kmalloc(pkt_len);

    create_tcp_packet(pkt, (void*)buffer, length,
                      socket->md.my_tcp_port,
                      socket->md.connect_tcp_port,
                      101, 101, TCP_FLAG_ACK | TCP_FLAG_PSH);

    send_ipv4_packet(socket->md.dest_ip, (uint8_t*)pkt, 6, pkt_len);
    kfree(pkt);
    return length;
}

int tcp_recv(struct net_socket* socket, void* buffer, size_t length) {
    if (!socket->md.connected) return -1;

    void* poll = kmalloc(1024 * 4);
    uint16_t data_len = 0;
    
    while (1) {
        rtl8139_poll(poll);
        struct eth_frame *frame = (struct eth_frame*)poll;
        if (ntohs(frame->ethertype) == 0x0800) {
            struct ipv4_packet *ipv4 = (struct ipv4_packet*)frame->payload;
            if (ipv4->hdr.protocol == 6) { 
                struct tcp_packet *tcp = (struct tcp_packet*)ipv4->payload;
                
                // Calculate size of TCP options / headers to locate raw data offset
                uint32_t header_len = (tcp->hdr.data_offset >> 4) * 4;
                uint32_t total_ip_len = ntohs(ipv4->hdr.total_length);
                uint32_t ip_header_len = (ipv4->hdr.version_ihl & 0x0F) * 4;
                
                data_len = total_ip_len - ip_header_len - header_len;
                
                if (data_len > 0) {
                    if (data_len > length) {
                        data_len = length;
                    }
                    // tcp->hdr.data_offset location calculation
                    uint8_t* tcp_data_ptr = ((uint8_t*)tcp) + header_len;
                    memcpy(buffer, tcp_data_ptr, data_len);
                    break;
                }
            }
        }
    }
    kfree(poll);
    return data_len;
}

int tcp_close(struct net_socket* socket) {
    if (!socket->md.connected) return 0;

    size_t pkt_len = sizeof(struct tcp_packet);
    struct tcp_packet *pkt = kmalloc(pkt_len);

    create_tcp_packet(pkt, NULL, 0, 
                      socket->md.my_tcp_port, 
                      socket->md.connect_tcp_port, 
                      102, 102, TCP_FLAG_FIN | TCP_FLAG_ACK);

    send_ipv4_packet(socket->md.dest_ip, (uint8_t*)pkt, 6, pkt_len);
    kfree(pkt);

    socket->md.connected = 0;
    return 0;
}
struct net_socket sockets[64];
int sock(net_family_t family, net_protocol_t protocol) {
    int available_socket = ++last_socket; // Increments to accurate next index
    
    sockets[available_socket].family = family;
    sockets[available_socket].protocol = protocol;
    sockets[available_socket].md.index = available_socket;
    sockets[available_socket].md.connected = 0;

    if (protocol == NET_PROTO_UDP) {
        sockets[available_socket].close = udp_close;
        sockets[available_socket].connect = udp_connect;
        sockets[available_socket].recv = udp_recv;
        sockets[available_socket].send = udp_send;
    } 
    else if (protocol == NET_PROTO_TCP) {
        sockets[available_socket].close = tcp_close;
        sockets[available_socket].connect = tcp_connect;
        sockets[available_socket].recv = tcp_recv;
        sockets[available_socket].send = tcp_send;
    } 
    else if (protocol == NET_PROTO_RAW) {
        sockets[available_socket].close = raw_close;
        sockets[available_socket].connect = raw_connect;
        sockets[available_socket].recv = raw_recv;
        sockets[available_socket].send = raw_send;
    }
    
    return available_socket;
}