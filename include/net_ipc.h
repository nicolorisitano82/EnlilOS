/*
 * EnlilOS - NET IPC protocol (M11-05)
 *
 * Protocollo minimo tra kernel socket layer e netd per i socket AF_INET
 * non-loopback. Il loopback locale continua a usare il backend kernel-side.
 */

#ifndef ENLILOS_NET_IPC_H
#define ENLILOS_NET_IPC_H

#include "types.h"

#define NETD_IO_BYTES  192U

typedef enum {
    NETD_REQ_SOCKET = 1,
    NETD_REQ_BIND = 2,
    NETD_REQ_CONNECT = 3,
    NETD_REQ_SEND = 4,
    NETD_REQ_RECV = 5,
    NETD_REQ_CLOSE = 6,
    NETD_REQ_POLL = 7,
    NETD_REQ_GETSOCKOPT = 8,
    NETD_REQ_SETSOCKOPT = 9,
    NETD_REQ_ADDR = 10,
    NETD_REQ_SHUTDOWN = 11,
} netd_req_op_t;

typedef struct {
    uint32_t op;
    int32_t  handle;
    uint32_t flags;
    uint32_t count;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint8_t  data[NETD_IO_BYTES];
} netd_request_t;

typedef struct {
    int32_t  status;
    int32_t  handle;
    uint32_t data_len;
    uint32_t flags;
    uint32_t addr_ip;
    uint16_t addr_port;
    uint16_t _pad0;
    uint32_t aux_ip;
    uint16_t aux_port;
    uint16_t _pad1;
    uint8_t  data[NETD_IO_BYTES];
} netd_response_t;

#endif /* ENLILOS_NET_IPC_H */
