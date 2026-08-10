#pragma once
#include <stdint.h>
#include <stddef.h>
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define htons(x) __builtin_bswap16(x)
    #define htonl(x) __builtin_bswap32(x)
    #define ntohs(x) __builtin_bswap16(x)
    #define ntohl(x) __builtin_bswap32(x)
#else
    #define htons(x) (x)
    #define htonl(x) (x)
    #define ntohs(x) (x)
    #define ntohl(x) (x)
#endif
typedef enum {
    NET_AF_IPV4,
    NET_AF_IPV6,
    NET_AF_ARP,
    NET_AF_RAW,
    NET_AF_UNIX
} net_family_t;
typedef enum {
    NET_PROTO_TCP,
    NET_PROTO_UDP,
    NET_PROTO_ICMP,
    NET_PROTO_RAW,
    NET_PROTO_UNIX
} net_protocol_t;
typedef struct net_socket net_socket;
typedef struct {
    uint64_t connect_udp_port;
    uint64_t connect_tcp_port;
    uint64_t my_udp_port;
    uint64_t my_tcp_port;
    uint32_t dest_ip;
    int index;
    uint8_t connected;
    uint8_t listening;
    char path[256];
    int unix_endpoint;
} metadata;
typedef struct net_addr {
    uint32_t ipv4;      // Network byte order
    uint16_t port;      // Network byte order
} net_addr;
typedef int (*net_recv_t)(
    struct net_socket *socket,
    void *buffer,
    size_t length
);

typedef int (*net_send_t)(
    struct net_socket *socket,
    const void *buffer,
    size_t length
);

typedef int (*net_connect_t)(
    struct net_socket *socket,
    const char *addr
);

typedef int (*net_listen_t)(
    struct net_socket *socket,
    const char *addr
);

typedef int (*net_accept_t)(
    struct net_socket *socket,
    struct net_socket *out_client
);

typedef int (*net_close_t)(
    struct net_socket *socket
);

typedef int (*net_bind_t)(
    struct net_socket *socket,
    const char *addr
);

typedef struct {
    char scheme[16];    // "http", "https", etc.
    char host[256];     // "example.com" or "192.168.1.10"
    uint16_t port;      // 80, 443, etc.
    char path[256];     // "/index.html"
} net_url_t;
struct net_socket {
    net_family_t family;
    net_protocol_t protocol;
    net_recv_t recv;
    net_close_t close;
    net_connect_t connect;
    net_send_t send;
    net_bind_t bind;
    net_listen_t listen;
    net_accept_t accept;
    metadata md;
};
int sock(net_family_t family, net_protocol_t protocol);
extern struct net_socket sockets[64];