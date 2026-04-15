#ifndef ENLILOS_MUSL_ARPA_INET_H
#define ENLILOS_MUSL_ARPA_INET_H

#include <netinet/in.h>
#include <sys/socket.h>
#include <stdint.h>

in_addr_t   inet_addr(const char *cp);
int         inet_aton(const char *cp, struct in_addr *inp);
char       *inet_ntoa(struct in_addr in);

#endif /* ENLILOS_MUSL_ARPA_INET_H */
