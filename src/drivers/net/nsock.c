#include <stdint.h>
#include <stddef.h>
#include <hals/net/RTL8139.h>
#include <drivers/net/HTTP.h>
#include <drivers/net/IPV4.h>
#include <drivers/net/TCP.h>
#include <drivers/net/UDP.h>
#include <drivers/net/nsock.h>

#include "string.h"
#include "drivers/alloc.h"

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

// Binds a UDP socket to a local port without marking it as listening.
// Can be followed by listen() or used for sending.
int udp_bind(struct net_socket* socket, const char* addr) {
    net_url_t url;
    net_parse_url(addr, &url);

    socket->md.my_udp_port = url.port;
    socket->md.dest_ip = 0;
    socket->md.path[0] = '\0';

    return 0;
}

// Binds a UDP socket to a local port without connecting to a remote
// peer, so it can receive datagrams from anyone.
int udp_listen(struct net_socket* socket, const char* addr) {
    net_url_t url;
    net_parse_url(addr, &url);

    socket->md.my_udp_port = url.port;
    socket->md.connect_udp_port = 0;
    socket->md.connected = 0;
    socket->md.listening = 1;
    socket->md.dest_ip = 0;
    socket->md.path[0] = '\0';

    return 0;
}

// Blocks until a UDP datagram arrives on this socket's bound port,
// then snapshots the sender into a fresh "connected" socket.
int udp_accept(struct net_socket* socket, struct net_socket* out_client) {
    if (!socket->md.listening) return -1;

    void* poll = kmalloc(1024 * 4);
    while (1) {
        rtl8139_poll(poll);
        struct eth_frame *frame = (struct eth_frame*)poll;
        if (ntohs(frame->ethertype) == 0x0800) {
            struct ipv4_packet *ipv4 = (struct ipv4_packet*)frame->payload;
            if (ipv4->hdr.protocol == 17) {
                struct udp_packet *udp = (struct udp_packet*)ipv4->payload;
                if (ntohs(udp->hdr.dst_port) == socket->md.my_udp_port) {
                    *out_client = *socket;
                    out_client->md.index = ++last_socket;
                    out_client->md.connect_udp_port = ntohs(udp->hdr.src_port);
                    out_client->md.dest_ip = ipv4->hdr.src_ip;
                    out_client->md.connected = 1;
                    out_client->md.listening = 0;
                    sockets[out_client->md.index] = *out_client;
                    break;
                }
            }
        }
    }
    kfree(poll);
    return out_client->md.index;
}

int udp_close(struct net_socket* socket) {
    socket->md.listening = 0;
    socket->md.connected = 0;
    return 0;
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
        if (ntohs(frame->ethertype) == 0x0800) {
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

    size_t pkt_len = sizeof(struct tcp_packet);
    struct tcp_packet *syn_pkt = kmalloc(pkt_len);

    create_tcp_packet(syn_pkt, NULL, 0,
                      socket->md.my_tcp_port,
                      socket->md.connect_tcp_port,
                      100, 0, TCP_FLAG_SYN);

    send_ipv4_packet(socket->md.dest_ip, (uint8_t*)syn_pkt, 6, pkt_len);
    kfree(syn_pkt);

    void* poll = kmalloc(1024 * 4);
    while (1) {
        rtl8139_poll(poll);
        struct eth_frame *frame = (struct eth_frame*)poll;
        if (ntohs(frame->ethertype) == 0x0800) {
            struct ipv4_packet *ipv4 = (struct ipv4_packet*)frame->payload;
            if (ipv4->hdr.protocol == 6) {
                struct tcp_packet *tcp = (struct tcp_packet*)ipv4->payload;
                if ((tcp->hdr.flags & TCP_FLAG_SYN) && (tcp->hdr.flags & TCP_FLAG_ACK)) {
                    break;
                }
            }
        }
    }
    kfree(poll);

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

                uint32_t header_len = (tcp->hdr.data_offset >> 4) * 4;
                uint32_t total_ip_len = ntohs(ipv4->hdr.total_length);
                uint32_t ip_header_len = (ipv4->hdr.version_ihl & 0x0F) * 4;

                data_len = total_ip_len - ip_header_len - header_len;

                if (data_len > 0) {
                    if (data_len > length) {
                        data_len = length;
                    }
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

// Binds this socket to a local TCP port without marking it as passive yet.
// Can be followed by listen() to start accepting connections.
int tcp_bind(struct net_socket* socket, const char* addr) {
    net_url_t url;
    net_parse_url(addr, &url);

    socket->md.my_tcp_port = url.port;
    socket->md.dest_ip = 0;

    return 0;
}

// Binds this socket to a local TCP port and marks it passive. No
// packets go out yet; actual SYN handling happens in tcp_accept.
int tcp_listen(struct net_socket* socket, const char* addr) {
    net_url_t url;
    net_parse_url(addr, &url);

    socket->md.my_tcp_port = url.port;
    socket->md.connect_tcp_port = 0;
    socket->md.connected = 0;
    socket->md.listening = 1;
    socket->md.dest_ip = 0;

    return 0;
}

// Blocks until a SYN arrives on the bound port, completes the
// three-way handshake, and returns a new connected socket for the
// accepted peer.
int tcp_accept(struct net_socket* socket, struct net_socket* out_client) {
    if (!socket->md.listening) return -1;

    void* poll = kmalloc(1024 * 4);
    struct ipv4_packet *matched_ipv4 = NULL;
    struct tcp_packet *matched_tcp = NULL;

    while (1) {
        rtl8139_poll(poll);
        struct eth_frame *frame = (struct eth_frame*)poll;
        if (ntohs(frame->ethertype) == 0x0800) {
            struct ipv4_packet *ipv4 = (struct ipv4_packet*)frame->payload;
            if (ipv4->hdr.protocol == 6) {
                struct tcp_packet *tcp = (struct tcp_packet*)ipv4->payload;
                if ((tcp->hdr.flags & TCP_FLAG_SYN) &&
                    !(tcp->hdr.flags & TCP_FLAG_ACK) &&
                    ntohs(tcp->hdr.dst_port) == socket->md.my_tcp_port) {
                    matched_ipv4 = ipv4;
                    matched_tcp = tcp;
                    break;
                }
            }
        }
    }

    *out_client = *socket;
    out_client->md.index = ++last_socket;
    out_client->md.connect_tcp_port = ntohs(matched_tcp->hdr.src_port);
    out_client->md.my_tcp_port = socket->md.my_tcp_port;
    out_client->md.dest_ip = matched_ipv4->hdr.src_ip;
    out_client->md.listening = 0;

    // Send SYN-ACK
    size_t pkt_len = sizeof(struct tcp_packet);
    struct tcp_packet *synack_pkt = kmalloc(pkt_len);
    create_tcp_packet(synack_pkt, NULL, 0,
                      out_client->md.my_tcp_port,
                      out_client->md.connect_tcp_port,
                      200, 101, TCP_FLAG_SYN | TCP_FLAG_ACK);
    send_ipv4_packet(out_client->md.dest_ip, (uint8_t*)synack_pkt, 6, pkt_len);
    kfree(synack_pkt);

    // Wait for final ACK from client to complete handshake
    while (1) {
        rtl8139_poll(poll);
        struct eth_frame *frame = (struct eth_frame*)poll;
        if (ntohs(frame->ethertype) == 0x0800) {
            struct ipv4_packet *ipv4 = (struct ipv4_packet*)frame->payload;
            if (ipv4->hdr.protocol == 6) {
                struct tcp_packet *tcp = (struct tcp_packet*)ipv4->payload;
                if ((tcp->hdr.flags & TCP_FLAG_ACK) &&
                    ntohs(tcp->hdr.dst_port) == out_client->md.my_tcp_port &&
                    ntohs(tcp->hdr.src_port) == out_client->md.connect_tcp_port) {
                    break;
                }
            }
        }
    }
    kfree(poll);

    out_client->md.connected = 1;
    sockets[out_client->md.index] = *out_client;
    return out_client->md.index;
}

int tcp_close(struct net_socket* socket) {
    if (!socket->md.connected) {
        socket->md.listening = 0;
        return 0;
    }

    size_t pkt_len = sizeof(struct tcp_packet);
    struct tcp_packet *pkt = kmalloc(pkt_len);

    create_tcp_packet(pkt, NULL, 0,
                      socket->md.my_tcp_port,
                      socket->md.connect_tcp_port,
                      102, 102, TCP_FLAG_FIN | TCP_FLAG_ACK);

    send_ipv4_packet(socket->md.dest_ip, (uint8_t*)pkt, 6, pkt_len);
    kfree(pkt);

    socket->md.connected = 0;
    socket->md.listening = 0;
    return 0;
}

// ==========================================
// AF_UNIX PROTOCOL HANDLERS
// ==========================================
//
// AF_UNIX sockets never touch the NIC. They're implemented as small
// in-kernel ring buffers keyed by a filesystem-style path string.
// A "connect" on a UNIX socket just looks up (or lazily creates) the
// named endpoint and binds this socket to it. send()/recv() push and
// pop bytes directly through that shared buffer. "listen" marks an
// endpoint as passive/accepting; "accept" blocks until a peer
// connect()s to that same path, then hands back a socket bound to
// a fresh, private ring buffer for that pair.
//
// IMPORTANT: none of these handlers may call rtl8139_poll(). That
// function pumps the NIC driver and has nothing to do with AF_UNIX
// traffic; calling it here was a straight copy/paste artifact from
// the UDP/TCP/raw handlers above, and it's actively harmful for
// AF_UNIX sockets:
//   - It requires a NIC to exist at all, so on network-less machines
//     (or once the NIC has been shut down) it can crash or hang.
//   - The busy-wait loops in unix_connect()/unix_accept()/unix_recv()
//     need to "just spin/yield" while another *local* thread makes
//     progress; polling and parsing Ethernet/IPv4 frames for that is
//     pure waste and, worse, means the "did the peer show up yet?"
//     condition can spuriously depend on unrelated network traffic
//     arriving to give the poll loop a chance to run.
// All rtl8139_poll() calls have been removed from the AF_UNIX section
// and replaced with cpu_relax(), a small architecture-provided
// busy-wait hint (falls back to a no-op spin if unavailable) that
// only yields the CPU, with no networking side effects whatsoever.

#define UNIX_MAX_ENDPOINTS   32
#define UNIX_PATH_MAX        108   // matches sockaddr_un convention
#define UNIX_BUF_SIZE        4096

#if defined(__has_include)
#  if __has_include(<hals/cpu.h>)
#    include <hals/cpu.h>
#    define UNIX_HAVE_CPU_RELAX 1
#  endif
#endif

#ifndef UNIX_HAVE_CPU_RELAX
static inline void cpu_relax(void) {
    /* No architecture-provided relax/pause hint available in this build;
     * fall back to a plain no-op spin. Still strictly better than the
     * previous behavior, which incorrectly pumped the NIC driver here. */
    (void)0;
}
#endif

typedef struct {
    char     path[UNIX_PATH_MAX];
    int      in_use;

    uint8_t  buffer[UNIX_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;

    int      bound_socket;   // index of socket currently attached, or -1

    int      listening;      // 1 if this endpoint is a passive listener
    int      pending_client; // index of a socket waiting to be accept()ed, or -1
} unix_endpoint_t;

static unix_endpoint_t unix_endpoints[UNIX_MAX_ENDPOINTS];
static int unix_endpoints_initialized = 0;

static void unix_endpoints_init(void) {
    if (unix_endpoints_initialized) return;
    for (int i = 0; i < UNIX_MAX_ENDPOINTS; i++) {
        unix_endpoints[i].in_use = 0;
        unix_endpoints[i].head = 0;
        unix_endpoints[i].tail = 0;
        unix_endpoints[i].count = 0;
        unix_endpoints[i].bound_socket = -1;
        unix_endpoints[i].listening = 0;
        unix_endpoints[i].pending_client = -1;
        unix_endpoints[i].path[0] = '\0';
    }
    unix_endpoints_initialized = 1;
}

static int unix_find_endpoint(const char *path) {
    for (int i = 0; i < UNIX_MAX_ENDPOINTS; i++) {
        if (unix_endpoints[i].in_use && !strcmp(unix_endpoints[i].path, path)) {
            return i;
        }
    }
    return -1;
}

static int unix_create_endpoint(const char *path) {
    for (int i = 0; i < UNIX_MAX_ENDPOINTS; i++) {
        if (!unix_endpoints[i].in_use) {
            unix_endpoints[i].in_use = 1;
            unix_endpoints[i].head = 0;
            unix_endpoints[i].tail = 0;
            unix_endpoints[i].count = 0;
            unix_endpoints[i].bound_socket = -1;
            unix_endpoints[i].listening = 0;
            unix_endpoints[i].pending_client = -1;
            strncpy(unix_endpoints[i].path, path, UNIX_PATH_MAX - 1);
            unix_endpoints[i].path[UNIX_PATH_MAX - 1] = '\0';
            return i;
        }
    }
    return -1;
}

// Extracts the socket path out of an addr string. AF_UNIX addresses
// aren't URLs, so we accept both a bare path ("/tmp/mysock") and a
// "unix:///tmp/mysock" style scheme for consistency with net_parse_url callers.
static void unix_extract_path(const char *addr, char *out, size_t out_size) {
    const char *p = addr;
    const char *prefix = "unix://";
    size_t prefix_len = 7;

    int matches = 1;
    for (size_t i = 0; i < prefix_len; i++) {
        if (p[i] != prefix[i]) { matches = 0; break; }
        if (p[i] == '\0') { matches = 0; break; }
    }

    if (matches) {
        p += prefix_len;
    }

    size_t i = 0;
    while (p[i] && i < out_size - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

int unix_connect(struct net_socket* socket, const char* addr) {
    unix_endpoints_init();

    char path[UNIX_PATH_MAX];
    unix_extract_path(addr, path, UNIX_PATH_MAX);

    int idx = unix_find_endpoint(path);
    if (idx < 0) {
        idx = unix_create_endpoint(path);
        if (idx < 0) {
            return -1; // no free endpoints
        }
    }

    // If a listener is waiting on this path, hand ourselves to it and
    // block until the listener's accept() picks us up and gives us a
    // private endpoint to talk through.
    if (unix_endpoints[idx].listening) {
        unix_endpoints[idx].pending_client = socket->md.index;

        while (unix_endpoints[idx].pending_client == socket->md.index) {
            cpu_relax(); // yield while the listener notices us
        }

        // By now socket->md.unix_endpoint has been rewritten by
        // unix_accept() to point at our private paired endpoint.
        socket->md.connected = 1;
        return 0;
    }

    unix_endpoints[idx].bound_socket = socket->md.index;

    strcpy(socket->md.path, path);
    socket->md.unix_endpoint = idx;
    socket->md.connected = 1;

    return 0;
}

// Binds a Unix domain socket to a path without marking it as listening.
// Can be followed by listen() to start accepting connections.
int unix_bind(struct net_socket* socket, const char* addr) {
    unix_endpoints_init();

    char path[UNIX_PATH_MAX];
    unix_extract_path(addr, path, UNIX_PATH_MAX);

    int idx = unix_find_endpoint(path);
    if (idx < 0) {
        idx = unix_create_endpoint(path);
        if (idx < 0) {
            return -1;
        }
    }

    strcpy(socket->md.path, path);
    socket->md.unix_endpoint = idx;
    socket->md.connected = 0;
    socket->md.listening = 0;

    return 0;
}

// Marks the endpoint at `addr` as a passive listener. Does not block;
// actual pairing happens in unix_accept().
int unix_listen(struct net_socket* socket, const char* addr) {
    unix_endpoints_init();

    char path[UNIX_PATH_MAX];
    unix_extract_path(addr, path, UNIX_PATH_MAX);

    int idx = unix_find_endpoint(path);
    if (idx < 0) {
        idx = unix_create_endpoint(path);
        if (idx < 0) {
            return -1;
        }
    }

    unix_endpoints[idx].listening = 1;
    unix_endpoints[idx].pending_client = -1;

    strcpy(socket->md.path, path);
    socket->md.unix_endpoint = idx;
    socket->md.listening = 1;
    socket->md.connected = 0;

    return 0;
}

// Blocks until some socket connect()s to our listening path, then
// gives that peer (and us) a fresh private ring buffer to talk
// through, and returns a new connected socket representing our end
// of the pair.
int unix_accept(struct net_socket* socket, struct net_socket* out_client) {
    if (!socket->md.listening) return -1;

    int listen_idx = socket->md.unix_endpoint;
    if (listen_idx < 0 || listen_idx >= UNIX_MAX_ENDPOINTS ||
        !unix_endpoints[listen_idx].in_use) {
        return -1;
    }

    // Wait for a connecting socket to register itself.
    while (unix_endpoints[listen_idx].pending_client == -1) {
        cpu_relax();
    }

    int client_sock_idx = unix_endpoints[listen_idx].pending_client;

    // Create a fresh private endpoint for this pair to use, distinct
    // from the well-known listening path, so other connect()s can
    // still queue up against the original path.
    char pair_path[UNIX_PATH_MAX];
    // A simple synthetic path derived from the listen path + counter.
    static int pair_counter = 0;
    size_t base_len = strlen(unix_endpoints[listen_idx].path);
    size_t i;
    for (i = 0; i < base_len && i < UNIX_PATH_MAX - 1; i++) {
        pair_path[i] = unix_endpoints[listen_idx].path[i];
    }
    // Append "#N"
    if (i < UNIX_PATH_MAX - 1) pair_path[i++] = '#';
    int n = pair_counter++;
    char digits[10];
    int dcount = 0;
    if (n == 0) {
        digits[dcount++] = '0';
    } else {
        while (n > 0 && dcount < 10) {
            digits[dcount++] = '0' + (n % 10);
            n /= 10;
        }
    }
    while (dcount > 0 && i < UNIX_PATH_MAX - 1) {
        pair_path[i++] = digits[--dcount];
    }
    pair_path[i] = '\0';

    int pair_idx = unix_create_endpoint(pair_path);
    if (pair_idx < 0) {
        return -1; // no free endpoints
    }
    unix_endpoints[pair_idx].bound_socket = socket->md.index;

    // Build the accepted socket (our side of the pair).
    *out_client = *socket;
    out_client->md.index = ++last_socket;
    out_client->md.unix_endpoint = pair_idx;
    out_client->md.listening = 0;
    out_client->md.connected = 1;
    strcpy(out_client->md.path, pair_path);
    sockets[out_client->md.index] = *out_client;

    // Rewire the waiting client socket to use the same private
    // endpoint, and release it from its spin-wait in unix_connect().
    sockets[client_sock_idx].md.unix_endpoint = pair_idx;
    strcpy(sockets[client_sock_idx].md.path, pair_path);
    unix_endpoints[pair_idx].bound_socket = client_sock_idx;

    unix_endpoints[listen_idx].pending_client = -1;

    return out_client->md.index;
}

int unix_send(struct net_socket* socket, const void* buffer, size_t length) {
    if (!socket->md.connected) return -1;

    int idx = socket->md.unix_endpoint;
    if (idx < 0 || idx >= UNIX_MAX_ENDPOINTS || !unix_endpoints[idx].in_use) {
        return -1;
    }

    unix_endpoint_t *ep = &unix_endpoints[idx];
    const uint8_t *src = (const uint8_t*)buffer;
    size_t written = 0;

    for (size_t i = 0; i < length; i++) {
        if (ep->count >= UNIX_BUF_SIZE) {
            // buffer full, drop the rest (no blocking implemented)
            break;
        }
        ep->buffer[ep->head] = src[i];
        ep->head = (ep->head + 1) % UNIX_BUF_SIZE;
        ep->count++;
        written++;
    }

    return (int)written;
}

int unix_recv(struct net_socket* socket, void* buffer, size_t length) {
    if (!socket->md.connected) return -1;

    int idx = socket->md.unix_endpoint;
    if (idx < 0 || idx >= UNIX_MAX_ENDPOINTS || !unix_endpoints[idx].in_use) {
        return -1;
    }

    unix_endpoint_t *ep = &unix_endpoints[idx];
    uint8_t *dst = (uint8_t*)buffer;
    size_t read_count = 0;

    // Busy-poll until at least one byte is available, matching the
    // blocking style of the other recv() handlers in this file.
    // Note: this only spins the CPU (cpu_relax) -- it does NOT pump
    // the NIC driver, since AF_UNIX traffic never touches the network.
    while (ep->count == 0) {
        cpu_relax();
    }

    while (read_count < length && ep->count > 0) {
        dst[read_count] = ep->buffer[ep->tail];
        ep->tail = (ep->tail + 1) % UNIX_BUF_SIZE;
        ep->count--;
        read_count++;
    }

    return (int)read_count;
}

int unix_close(struct net_socket* socket) {
    if (!socket->md.connected && !socket->md.listening) return 0;

    int idx = socket->md.unix_endpoint;
    if (idx >= 0 && idx < UNIX_MAX_ENDPOINTS && unix_endpoints[idx].in_use) {
        if (unix_endpoints[idx].bound_socket == socket->md.index) {
            unix_endpoints[idx].bound_socket = -1;
        }
        if (socket->md.listening) {
            unix_endpoints[idx].listening = 0;
            unix_endpoints[idx].pending_client = -1;
        }
        // Endpoint itself stays allocated (path remains bindable) until
        // no sockets reference it; simple policy here: free once unbound
        // and not listening.
        if (unix_endpoints[idx].bound_socket == -1 && !unix_endpoints[idx].listening) {
            unix_endpoints[idx].in_use = 0;
            unix_endpoints[idx].head = 0;
            unix_endpoints[idx].tail = 0;
            unix_endpoints[idx].count = 0;
            unix_endpoints[idx].path[0] = '\0';
        }
    }

    socket->md.unix_endpoint = -1;
    socket->md.connected = 0;
    socket->md.listening = 0;
    return 0;
}

struct net_socket sockets[64];
int sock(net_family_t family, net_protocol_t protocol) {
    int available_socket = ++last_socket; // Increments to accurate next index

    sockets[available_socket].family = family;
    sockets[available_socket].protocol = protocol;
    sockets[available_socket].md.index = available_socket;
    sockets[available_socket].md.connected = 0;
    sockets[available_socket].md.listening = 0;
    sockets[available_socket].md.unix_endpoint = -1;

    if (protocol == NET_PROTO_UDP) {
        sockets[available_socket].close = udp_close;
        sockets[available_socket].connect = udp_connect;
        sockets[available_socket].bind = udp_bind;
        sockets[available_socket].listen = udp_listen;
        sockets[available_socket].accept = udp_accept;
        sockets[available_socket].recv = udp_recv;
        sockets[available_socket].send = udp_send;
    }
    else if (protocol == NET_PROTO_TCP) {
        sockets[available_socket].close = tcp_close;
        sockets[available_socket].connect = tcp_connect;
        sockets[available_socket].bind = tcp_bind;
        sockets[available_socket].listen = tcp_listen;
        sockets[available_socket].accept = tcp_accept;
        sockets[available_socket].recv = tcp_recv;
        sockets[available_socket].send = tcp_send;
    }
    else if (protocol == NET_PROTO_RAW) {
        sockets[available_socket].close = raw_close;
        sockets[available_socket].connect = raw_connect;
        sockets[available_socket].recv = raw_recv;
        sockets[available_socket].send = raw_send;
    }
    else if (family == NET_AF_UNIX) {
        sockets[available_socket].close = unix_close;
        sockets[available_socket].connect = unix_connect;
        sockets[available_socket].bind = unix_bind;
        sockets[available_socket].listen = unix_listen;
        sockets[available_socket].accept = unix_accept;
        sockets[available_socket].recv = unix_recv;
        sockets[available_socket].send = unix_send;
    }

    return available_socket;
}