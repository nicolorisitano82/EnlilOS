#ifndef ENLILOS_MUSL_WCHAR_H
#define ENLILOS_MUSL_WCHAR_H

#include <stddef.h>
#include <sys/types.h>

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
int    mbsinit(const mbstate_t *ps);

#endif
