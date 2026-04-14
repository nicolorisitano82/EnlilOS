#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static int is_leap(int year)
{
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static int month_days(int year, int month)
{
    static const int days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month == 1 && is_leap(year))
        return 29;
    return days[month];
}

time_t time(time_t *tloc)
{
    struct timeval tv;
    time_t         now = 0;

    if (gettimeofday(&tv, NULL) == 0)
        now = (time_t)tv.tv_sec;
    if (tloc)
        *tloc = now;
    return now;
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
    long      days;
    long      rem;
    int       year = 1970;
    int       month = 0;
    long      yday = 0;

    if (!timep || !result)
        return NULL;

    memset(result, 0, sizeof(*result));
    rem = (long)(*timep % 86400L);
    if (rem < 0L)
        rem += 86400L;
    days = (long)(*timep / 86400L);
    if (*timep < 0 && (*timep % 86400L) != 0)
        days--;

    result->tm_hour = (int)(rem / 3600L);
    rem %= 3600L;
    result->tm_min = (int)(rem / 60L);
    result->tm_sec = (int)(rem % 60L);
    result->tm_wday = (int)((days + 4L) % 7L);
    if (result->tm_wday < 0)
        result->tm_wday += 7;

    while (days < 0 || days >= (long)(is_leap(year) ? 366 : 365)) {
        long year_days = (long)(is_leap(year) ? 366 : 365);

        if (days < 0) {
            year--;
            days += (long)(is_leap(year) ? 366 : 365);
        } else {
            days -= year_days;
            year++;
        }
    }

    result->tm_year = year - 1900;
    result->tm_yday = (int)days;
    yday = days;

    for (month = 0; month < 12; month++) {
        int mdays = month_days(year, month);

        if (days < mdays)
            break;
        days -= mdays;
    }

    result->tm_mon = month;
    result->tm_mday = (int)days + 1;
    result->tm_isdst = 0;
    result->tm_yday = (int)yday;
    return result;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
    size_t used = 0U;

    if (!s || max == 0U || !format || !tm)
        return 0U;

    while (*format != '\0') {
        char chunk[32];
        int  len;

        if (*format != '%') {
            if (used + 1U >= max)
                return 0U;
            s[used++] = *format++;
            continue;
        }

        format++;
        switch (*format) {
        case '%':
            chunk[0] = '%';
            chunk[1] = '\0';
            break;
        case 'Y':
            snprintf(chunk, sizeof(chunk), "%04d", tm->tm_year + 1900);
            break;
        case 'm':
            snprintf(chunk, sizeof(chunk), "%02d", tm->tm_mon + 1);
            break;
        case 'd':
            snprintf(chunk, sizeof(chunk), "%02d", tm->tm_mday);
            break;
        case 'H':
            snprintf(chunk, sizeof(chunk), "%02d", tm->tm_hour);
            break;
        case 'M':
            snprintf(chunk, sizeof(chunk), "%02d", tm->tm_min);
            break;
        case 'S':
            snprintf(chunk, sizeof(chunk), "%02d", tm->tm_sec);
            break;
        default:
            chunk[0] = '%';
            chunk[1] = *format ? *format : '\0';
            chunk[2] = '\0';
            break;
        }

        len = (int)strlen(chunk);
        if (used + (size_t)len >= max)
            return 0U;
        memcpy(s + used, chunk, (size_t)len);
        used += (size_t)len;
        if (*format != '\0')
            format++;
    }

    s[used] = '\0';
    return used;
}
