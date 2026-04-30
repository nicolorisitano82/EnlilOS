#ifndef ENLILOS_MUSL_LOCALE_H
#define ENLILOS_MUSL_LOCALE_H

#include <stddef.h>

#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL      6

char *setlocale(int category, const char *locale);
size_t __locale_mb_cur_max(void);

#define MB_CUR_MAX (__locale_mb_cur_max())

#endif
