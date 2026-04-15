/*
 * EnlilOS musl bootstrap — BSD socket wrappers (M10-03)
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "user_svc.h"
#include "enlil_syscalls.h"

#define SYS_SOCKET      200
#define SYS_BIND        201
#define SYS_LISTEN      202
#define SYS_ACCEPT      203
#define SYS_CONNECT     204
#define SYS_SEND        205
#define SYS_RECV        206
#define SYS_SENDTO      207
#define SYS_RECVFROM    208
#define SYS_SETSOCKOPT  209
#define SYS_GETSOCKOPT  210
#define SYS_SHUTDOWN    211

static long sock_errno(long rc)
{
    if (rc < 0) {
        errno = (int)(-rc);
        return -1L;
    }
    return rc;
}

int socket(int domain, int type, int protocol)
{
    return (int)sock_errno(user_svc3(SYS_SOCKET, domain, type, protocol));
}

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    return (int)sock_errno(user_svc3(SYS_BIND, fd, (long)addr, (long)addrlen));
}

int listen(int fd, int backlog)
{
    return (int)sock_errno(user_svc2(SYS_LISTEN, fd, backlog));
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    return (int)sock_errno(user_svc3(SYS_ACCEPT, fd, (long)addr, (long)addrlen));
}

int accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    (void)flags;
    return accept(fd, addr, addrlen);
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    return (int)sock_errno(user_svc3(SYS_CONNECT, fd, (long)addr, (long)addrlen));
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return (ssize_t)sock_errno(user_svc4(SYS_SEND, fd, (long)buf, (long)len, flags));
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return (ssize_t)sock_errno(user_svc4(SYS_RECV, fd, (long)buf, (long)len, flags));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    return (ssize_t)sock_errno(
        user_svc6(SYS_SENDTO, fd, (long)buf, (long)len, flags,
                  (long)dest_addr, (long)addrlen));
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    return (ssize_t)sock_errno(
        user_svc6(SYS_RECVFROM, fd, (long)buf, (long)len, flags,
                  (long)src_addr, (long)addrlen));
}

int setsockopt(int fd, int level, int optname,
               const void *optval, socklen_t optlen)
{
    return (int)sock_errno(
        user_svc5(SYS_SETSOCKOPT, fd, level, optname,
                  (long)optval, (long)optlen));
}

int getsockopt(int fd, int level, int optname,
               void *optval, socklen_t *optlen)
{
    return (int)sock_errno(
        user_svc5(SYS_GETSOCKOPT, fd, level, optname,
                  (long)optval, (long)optlen));
}

int shutdown(int fd, int how)
{
    return (int)sock_errno(user_svc2(SYS_SHUTDOWN, fd, how));
}

/* ── inet helpers ─────────────────────────────────────────────── */

/*
 * inet_addr("a.b.c.d") → valore in network byte order (big-endian).
 * Restituisce INADDR_NONE (0xffffffff) su errore.
 */
in_addr_t inet_addr(const char *cp)
{
    uint32_t result = 0U;
    int      shift  = 24;

    if (!cp)
        return (in_addr_t)0xFFFFFFFFU;

    while (shift >= 0) {
        uint32_t octet = 0U;

        while (*cp >= '0' && *cp <= '9')
            octet = octet * 10U + (uint32_t)(*cp++ - '0');

        if (octet > 255U)
            return (in_addr_t)0xFFFFFFFFU;

        result |= (octet << (uint32_t)shift);
        shift  -= 8;

        if (shift >= 0) {
            if (*cp != '.')
                return (in_addr_t)0xFFFFFFFFU;
            cp++;
        }
    }

    /* result è in host byte order; converti in network byte order */
    return (in_addr_t)htonl(result);
}

int inet_aton(const char *cp, struct in_addr *inp)
{
    in_addr_t v = inet_addr(cp);

    if (v == (in_addr_t)0xFFFFFFFFU)
        return 0;
    if (inp)
        inp->s_addr = v;
    return 1;
}

static char *ntoa_append_octet(char *p, unsigned n)
{
    if (n >= 100U) *p++ = (char)('0' + n / 100U);
    if (n >= 10U)  *p++ = (char)('0' + (n % 100U) / 10U);
    *p++ = (char)('0' + n % 10U);
    return p;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[16];
    uint32_t    v = ntohl(in.s_addr);
    unsigned    a = (v >> 24U) & 0xFFU;
    unsigned    b = (v >> 16U) & 0xFFU;
    unsigned    c = (v >>  8U) & 0xFFU;
    unsigned    d =  v         & 0xFFU;
    char       *p = buf;

    p = ntoa_append_octet(p, a); *p++ = '.';
    p = ntoa_append_octet(p, b); *p++ = '.';
    p = ntoa_append_octet(p, c); *p++ = '.';
    p = ntoa_append_octet(p, d); *p   = '\0';
    return buf;
}
