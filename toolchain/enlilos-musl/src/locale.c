#include <locale.h>
#include <stdlib.h>
#include <string.h>

static char g_locale_name[16] = "C.UTF-8";
static int g_locale_mb_cur_max = 4;

char *setlocale(int category, const char *locale)
{
    (void)category;

    if (!locale)
        return g_locale_name;

    if (*locale == '\0') {
        (void)strcpy(g_locale_name, "C.UTF-8");
        g_locale_mb_cur_max = 4;
        return g_locale_name;
    }

    if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0) {
        (void)strcpy(g_locale_name, "C");
        g_locale_mb_cur_max = 1;
        return g_locale_name;
    }

    if (strcmp(locale, "C.UTF-8") == 0 || strcmp(locale, "en_US.UTF-8") == 0) {
        (void)strcpy(g_locale_name, locale);
        g_locale_mb_cur_max = 4;
        return g_locale_name;
    }

    return NULL;
}

size_t __locale_mb_cur_max(void)
{
    return (size_t)g_locale_mb_cur_max;
}

int mblen(const char *s, size_t n)
{
    unsigned char c;

    if (!s)
        return 0;
    if (n == 0U)
        return -1;
    if (*s == '\0')
        return 0;

    c = (unsigned char)*s;
    if (c < 0x80U)
        return 1;
    if ((c & 0xE0U) == 0xC0U)
        return (n >= 2U) ? 2 : -1;
    if ((c & 0xF0U) == 0xE0U)
        return (n >= 3U) ? 3 : -1;
    if ((c & 0xF8U) == 0xF0U)
        return (n >= 4U) ? 4 : -1;
    return -1;
}
