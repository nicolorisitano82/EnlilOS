#ifndef ENLILOS_MUSL_TIME_H
#define ENLILOS_MUSL_TIME_H

#include <sys/types.h>

struct timespec {
    long tv_sec;
    long tv_nsec;
};

int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
