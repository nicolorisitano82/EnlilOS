#include <fnmatch.h>
#include <stddef.h>

static int fnm_is_leading_period(const char *string, const char *string_start,
                                 int flags)
{
    if (!(flags & FNM_PERIOD) || !string || *string != '.')
        return 0;
    if (string == string_start)
        return 1;
    return ((flags & FNM_PATHNAME) && string[-1] == '/');
}

static const char *fnm_find_class_end(const char *pattern, int flags)
{
    const char *p = pattern;

    if (*p == '!' || *p == '^')
        p++;
    if (*p == ']')
        p++;

    while (*p) {
        if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1] != '\0') {
            p += 2;
            continue;
        }
        if (*p == ']')
            return p;
        p++;
    }
    return NULL;
}

static int fnm_range_match(const char **patternp, unsigned char test, int flags)
{
    const char *pattern = *patternp;
    int         negate = 0;
    int         match = 0;

    if (*pattern == '!' || *pattern == '^') {
        negate = 1;
        pattern++;
    }
    if (*pattern == ']') {
        if (test == ']')
            match = 1;
        pattern++;
    }

    while (*pattern && *pattern != ']') {
        unsigned char start = (unsigned char)*pattern++;
        unsigned char end = start;

        if (start == '\\' && !(flags & FNM_NOESCAPE) && *pattern != '\0')
            start = (unsigned char)*pattern++;

        if (*pattern == '-' && pattern[1] != '\0' && pattern[1] != ']') {
            pattern++;
            end = (unsigned char)*pattern++;
            if (end == '\\' && !(flags & FNM_NOESCAPE) && *pattern != '\0')
                end = (unsigned char)*pattern++;
        }

        if (start <= test && test <= end)
            match = 1;
    }

    if (*pattern == ']')
        pattern++;

    *patternp = pattern;
    return negate ? !match : match;
}

static int fnm_match_here(const char *pattern, const char *string,
                          const char *string_start, int flags)
{
    while (*pattern) {
        unsigned char pc = (unsigned char)*pattern++;

        switch (pc) {
        case '?':
            if (*string == '\0')
                return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *string == '/')
                return FNM_NOMATCH;
            if (fnm_is_leading_period(string, string_start, flags))
                return FNM_NOMATCH;
            string++;
            break;

        case '*':
            while (*pattern == '*')
                pattern++;

            if (fnm_is_leading_period(string, string_start, flags))
                return FNM_NOMATCH;

            if (*pattern == '\0') {
                if (flags & FNM_PATHNAME) {
                    while (*string) {
                        if (*string == '/')
                            return FNM_NOMATCH;
                        string++;
                    }
                }
                return 0;
            }

            for (;;) {
                if (fnm_match_here(pattern, string, string_start, flags) == 0)
                    return 0;
                if (*string == '\0')
                    break;
                if ((flags & FNM_PATHNAME) && *string == '/')
                    break;
                string++;
            }
            return FNM_NOMATCH;

        case '[': {
            const char *class_end = fnm_find_class_end(pattern, flags);

            if (!class_end) {
                if (*string != '[')
                    return FNM_NOMATCH;
                string++;
                break;
            }
            if (*string == '\0')
                return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *string == '/')
                return FNM_NOMATCH;
            if (fnm_is_leading_period(string, string_start, flags))
                return FNM_NOMATCH;
            if (!fnm_range_match(&pattern, (unsigned char)*string, flags))
                return FNM_NOMATCH;
            string++;
            break;
        }

        case '\\':
            if (!(flags & FNM_NOESCAPE) && *pattern != '\0')
                pc = (unsigned char)*pattern++;
            /* fallthrough */
        default:
            if ((char)pc != *string)
                return FNM_NOMATCH;
            string++;
            break;
        }
    }

    return (*string == '\0') ? 0 : FNM_NOMATCH;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    if (!pattern || !string)
        return FNM_NOMATCH;
    return fnm_match_here(pattern, string, string, flags);
}
