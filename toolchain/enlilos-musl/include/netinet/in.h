#ifndef ENLILOS_MUSL_NETINET_IN_H
#define ENLILOS_MUSL_NETINET_IN_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;   /* network byte order */
};

struct sockaddr_in {
    sa_family_t    sin_family;   /* AF_INET */
    in_port_t      sin_port;     /* network byte order */
    struct in_addr sin_addr;     /* network byte order */
    char           sin_zero[8];  /* padding */
};

/* Standard address constants — stored as host-integer NBO values */
#define INADDR_ANY          ((in_addr_t)0x00000000U)
#define INADDR_LOOPBACK     ((in_addr_t)0x7f000001U)
#define INADDR_BROADCAST    ((in_addr_t)0xffffffffU)
#define INADDR_NONE         ((in_addr_t)0xffffffffU)

/* Protocol numbers */
#define IPPROTO_IP          0
#define IPPROTO_ICMP        1
#define IPPROTO_TCP         6
#define IPPROTO_UDP         17

/* TCP options */
#define TCP_NODELAY         1
#define TCP_MAXSEG          2

/* Byte-order conversion (AArch64 is little-endian) */
static inline uint16_t htons(uint16_t v)
{
    return (uint16_t)((v >> 8U) | (v << 8U));
}

static inline uint16_t ntohs(uint16_t v)
{
    return (uint16_t)((v >> 8U) | (v << 8U));
}

static inline uint32_t htonl(uint32_t v)
{
    return ((v >> 24U) & 0x000000FFU)
         | ((v >>  8U) & 0x0000FF00U)
         | ((v <<  8U) & 0x00FF0000U)
         | ((v << 24U) & 0xFF000000U);
}

static inline uint32_t ntohl(uint32_t v)
{
    return ((v >> 24U) & 0x000000FFU)
         | ((v >>  8U) & 0x0000FF00U)
         | ((v <<  8U) & 0x00FF0000U)
         | ((v << 24U) & 0xFF000000U);
}

#endif /* ENLILOS_MUSL_NETINET_IN_H */
