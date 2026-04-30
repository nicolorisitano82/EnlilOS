#include <errno.h>

static __thread int __enlil_errno;

int *__errno_location(void) {
    return &__enlil_errno;
}
