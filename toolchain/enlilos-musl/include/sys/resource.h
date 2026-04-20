#ifndef ENLILOS_MUSL_SYS_RESOURCE_H
#define ENLILOS_MUSL_SYS_RESOURCE_H

#include <sys/types.h>
#include <stdint.h>

typedef uint64_t rlim64_t;

struct rlimit {
    rlim_t  rlim_cur;
    rlim_t  rlim_max;
};

struct rlimit64 {
    rlim64_t rlim_cur;
    rlim64_t rlim_max;
};

#define RLIM_INFINITY    ((rlim_t)~0UL)
#define RLIM64_INFINITY  ((rlim64_t)~0ULL)

#define RLIMIT_CPU       0
#define RLIMIT_FSIZE     1
#define RLIMIT_DATA      2
#define RLIMIT_STACK     3
#define RLIMIT_CORE      4
#define RLIMIT_RSS       5
#define RLIMIT_NPROC     6
#define RLIMIT_NOFILE    7
#define RLIMIT_MEMLOCK   8
#define RLIMIT_AS        9
#define RLIMIT_LOCKS     10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE  12
#define RLIMIT_NICE      13
#define RLIMIT_RTPRIO    14
#define RLIMIT_RTTIME    15
#define RLIMIT_NLIMITS   16

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getrlimit64(int resource, struct rlimit64 *rlim);
int setrlimit64(int resource, const struct rlimit64 *rlim);
int prlimit(pid_t pid, int resource, const struct rlimit *new_limit,
            struct rlimit *old_limit);
int prlimit64(pid_t pid, int resource, const struct rlimit64 *new_limit,
              struct rlimit64 *old_limit);

#endif
