#ifndef ENLILOS_MUSL_SYS_SOCKET_H
#define ENLILOS_MUSL_SYS_SOCKET_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

/* Address families */
#define AF_UNIX             1
#define AF_INET             2
#define AF_INET6            10
#define PF_UNIX             AF_UNIX
#define PF_INET             AF_INET
#define PF_INET6            AF_INET6

/* Socket types */
#define SOCK_STREAM         1
#define SOCK_DGRAM          2
#define SOCK_NONBLOCK       04000       /* O_NONBLOCK */
#define SOCK_CLOEXEC        02000000

/* SOL_SOCKET + option names */
#define SOL_SOCKET          1
#define SO_REUSEADDR        2
#define SO_KEEPALIVE        9
#define SO_SNDBUF           7
#define SO_RCVBUF           8
#define SO_ERROR            4

/* IPPROTO_TCP option */
#define TCP_NODELAY         1

/* shutdown(how) */
#define SHUT_RD             0
#define SHUT_WR             1
#define SHUT_RDWR           2

/* send/recv flags */
#define MSG_WAITALL         0x100
#define MSG_DONTWAIT        0x40
#define MSG_NOSIGNAL        0x4000

typedef unsigned int   socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        __ss_padding[126];
};

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int     accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags);
int     connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
int     setsockopt(int fd, int level, int optname,
                   const void *optval, socklen_t optlen);
int     getsockopt(int fd, int level, int optname,
                   void *optval, socklen_t *optlen);
int     shutdown(int fd, int how);

#endif /* ENLILOS_MUSL_SYS_SOCKET_H */
