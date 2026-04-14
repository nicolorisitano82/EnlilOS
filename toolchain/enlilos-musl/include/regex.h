#ifndef ENLILOS_MUSL_REGEX_H
#define ENLILOS_MUSL_REGEX_H

#include <stddef.h>

typedef long regoff_t;

typedef struct {
    char   *pattern;
    size_t  re_nsub;
    int     cflags;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED    0x0001
#define REG_ICASE       0x0002
#define REG_NOSUB       0x0004
#define REG_NEWLINE     0x0008

#define REG_NOMATCH     1
#define REG_BADPAT      2
#define REG_EBRACK      3
#define REG_ESPACE      4

int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string,
               size_t nmatch, regmatch_t pmatch[], int eflags);
void   regfree(regex_t *preg);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);

#endif
