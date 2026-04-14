#include <errno.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

static const char *regex_error_text(int errcode)
{
    switch (errcode) {
    case 0: return "success";
    case REG_NOMATCH: return "no match";
    case REG_BADPAT: return "unsupported regex pattern";
    case REG_EBRACK: return "unbalanced bracket expression";
    case REG_ESPACE: return "out of memory";
    default: return "regex error";
    }
}

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    size_t len;

    if (!preg || !pattern) {
        errno = EINVAL;
        return REG_BADPAT;
    }

    memset(preg, 0, sizeof(*preg));
    len = strlen(pattern);
    preg->pattern = (char *)malloc(len + 1U);
    if (!preg->pattern)
        return REG_ESPACE;

    memcpy(preg->pattern, pattern, len + 1U);
    preg->cflags = cflags;
    preg->re_nsub = 0U;
    return 0;
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags)
{
    const char *pattern;
    const char *found;
    size_t      pat_len;
    size_t      str_len;
    int         anchor_start = 0;
    int         anchor_end = 0;

    (void)eflags;

    if (!preg || !preg->pattern || !string)
        return REG_BADPAT;

    pattern = preg->pattern;
    if (pattern[0] == '^') {
        anchor_start = 1;
        pattern++;
    }
    pat_len = strlen(pattern);
    if (pat_len > 0U && pattern[pat_len - 1U] == '$') {
        anchor_end = 1;
        pat_len--;
    }

    if (pat_len == 0U) {
        if (nmatch > 0U && pmatch) {
            pmatch[0].rm_so = 0;
            pmatch[0].rm_eo = 0;
        }
        return 0;
    }

    str_len = strlen(string);
    if (anchor_start) {
        if (str_len < pat_len || strncmp(string, pattern, pat_len) != 0)
            return REG_NOMATCH;
        if (anchor_end && str_len != pat_len)
            return REG_NOMATCH;
        if (nmatch > 0U && pmatch) {
            pmatch[0].rm_so = 0;
            pmatch[0].rm_eo = (regoff_t)pat_len;
        }
        return 0;
    }

    found = strstr(string, pattern);
    if (!found)
        return REG_NOMATCH;

    if (anchor_end && (size_t)(found - string) + pat_len != str_len)
        return REG_NOMATCH;

    if (nmatch > 0U && pmatch) {
        pmatch[0].rm_so = (regoff_t)(found - string);
        pmatch[0].rm_eo = (regoff_t)(pmatch[0].rm_so + (regoff_t)pat_len);
    }
    return 0;
}

void regfree(regex_t *preg)
{
    if (!preg)
        return;
    free(preg->pattern);
    preg->pattern = NULL;
    preg->re_nsub = 0U;
    preg->cflags = 0;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    const char *text = regex_error_text(errcode);
    size_t      len = strlen(text) + 1U;

    (void)preg;

    if (errbuf && errbuf_size > 0U) {
        size_t copy = (len > errbuf_size) ? (errbuf_size - 1U) : (len - 1U);

        memcpy(errbuf, text, copy);
        errbuf[copy] = '\0';
    }
    return len;
}
