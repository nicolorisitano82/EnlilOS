#ifndef ENLILOS_MUSL_TIME_H
#define ENLILOS_MUSL_TIME_H

#include <sys/types.h>

struct timespec {
    long tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t      time(time_t *tloc);
struct tm  *localtime_r(const time_t *timep, struct tm *result);
size_t      strftime(char *s, size_t max, const char *format, const struct tm *tm);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
