#include <stddef.h>
#include <wchar.h>
#include <wctype.h>

static size_t decode_utf8(wchar_t *pwc, const unsigned char *s, size_t n)
{
    wchar_t wc;

    if (n == 0U)
        return (size_t)-2;

    if (s[0] < 0x80U) {
        wc = (wchar_t)s[0];
        if (pwc)
            *pwc = wc;
        return (s[0] == '\0') ? 0U : 1U;
    }

    if ((s[0] & 0xE0U) == 0xC0U) {
        if (n < 2U)
            return (size_t)-2;
        wc = (wchar_t)(((unsigned)s[0] & 0x1FU) << 6);
        wc |= (wchar_t)((unsigned)s[1] & 0x3FU);
        if (pwc)
            *pwc = wc;
        return 2U;
    }

    if ((s[0] & 0xF0U) == 0xE0U) {
        if (n < 3U)
            return (size_t)-2;
        wc = (wchar_t)(((unsigned)s[0] & 0x0FU) << 12);
        wc |= (wchar_t)(((unsigned)s[1] & 0x3FU) << 6);
        wc |= (wchar_t)((unsigned)s[2] & 0x3FU);
        if (pwc)
            *pwc = wc;
        return 3U;
    }

    if ((s[0] & 0xF8U) == 0xF0U) {
        if (n < 4U)
            return (size_t)-2;
        wc = (wchar_t)(((unsigned)s[0] & 0x07U) << 18);
        wc |= (wchar_t)(((unsigned)s[1] & 0x3FU) << 12);
        wc |= (wchar_t)(((unsigned)s[2] & 0x3FU) << 6);
        wc |= (wchar_t)((unsigned)s[3] & 0x3FU);
        if (pwc)
            *pwc = wc;
        return 4U;
    }

    return (size_t)-1;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    (void)ps;

    if (!s)
        return 0U;
    return decode_utf8(pwc, (const unsigned char *)s, n);
}

int mbsinit(const mbstate_t *ps)
{
    (void)ps;
    return 1;
}

wint_t towlower(wint_t wc)
{
    if (wc >= 'A' && wc <= 'Z')
        return wc - 'A' + 'a';
    return wc;
}

wint_t towupper(wint_t wc)
{
    if (wc >= 'a' && wc <= 'z')
        return wc - 'a' + 'A';
    return wc;
}
