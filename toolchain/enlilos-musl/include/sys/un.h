#ifndef ENLILOS_MUSL_SYS_UN_H
#define ENLILOS_MUSL_SYS_UN_H

#include <sys/socket.h>

#define UNIX_PATH_MAX 108

struct sockaddr_un {
    unsigned short sun_family;   /* AF_UNIX */
    char           sun_path[UNIX_PATH_MAX];
};

#endif /* ENLILOS_MUSL_SYS_UN_H */
