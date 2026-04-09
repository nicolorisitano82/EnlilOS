#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char   *buf;
    size_t  size;
    size_t  pos;
    int     count;
} fmt_buf_t;

static void fmt_putc(fmt_buf_t *out, char c)
{
    if (out->buf && out->size != 0U && out->pos + 1U < out->size)
        out->buf[out->pos] = c;
    out->pos++;
    out->count++;
}

static void fmt_puts(fmt_buf_t *out, const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        fmt_putc(out, *s++);
}

static void fmt_put_unsigned(fmt_buf_t *out, unsigned long value,
                             unsigned base, int uppercase)
{
    char buf[32];
    size_t len = 0U;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (value == 0UL) {
        fmt_putc(out, '0');
        return;
    }

    while (value != 0UL && len < sizeof(buf)) {
        buf[len++] = digits[value % base];
        value /= base;
    }
    while (len > 0U)
        fmt_putc(out, buf[--len]);
}

static void fmt_put_signed(fmt_buf_t *out, long value)
{
    unsigned long mag;

    if (value < 0L) {
        fmt_putc(out, '-');
        mag = (unsigned long)(-(value + 1L)) + 1UL;
    } else {
        mag = (unsigned long)value;
    }
    fmt_put_unsigned(out, mag, 10U, 0);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    fmt_buf_t out = { buf, size, 0U, 0 };

    while (*fmt) {
        int long_flag = 0;

        if (*fmt != '%') {
            fmt_putc(&out, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '%') {
            fmt_putc(&out, *fmt++);
            continue;
        }

        while (*fmt == 'l') {
            long_flag++;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            fmt_putc(&out, (char)va_arg(ap, int));
            break;
        case 's':
            fmt_puts(&out, va_arg(ap, const char *));
            break;
        case 'd':
        case 'i':
            if (long_flag > 0)
                fmt_put_signed(&out, va_arg(ap, long));
            else
                fmt_put_signed(&out, (long)va_arg(ap, int));
            break;
        case 'u':
            if (long_flag > 0)
                fmt_put_unsigned(&out, va_arg(ap, unsigned long), 10U, 0);
            else
                fmt_put_unsigned(&out, (unsigned long)va_arg(ap, unsigned int), 10U, 0);
            break;
        case 'x':
        case 'X':
            if (long_flag > 0)
                fmt_put_unsigned(&out, va_arg(ap, unsigned long), 16U, *fmt == 'X');
            else
                fmt_put_unsigned(&out, (unsigned long)va_arg(ap, unsigned int), 16U, *fmt == 'X');
            break;
        case 'p':
            fmt_puts(&out, "0x");
            fmt_put_unsigned(&out, (unsigned long)(uintptr_t)va_arg(ap, void *), 16U, 0);
            break;
        default:
            fmt_putc(&out, '%');
            if (*fmt)
                fmt_putc(&out, *fmt);
            break;
        }

        if (*fmt != '\0')
            fmt++;
    }

    if (buf && size != 0U) {
        size_t end = (out.pos < size - 1U) ? out.pos : (size - 1U);
        buf[end] = '\0';
    }

    return out.count;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return rc;
}

int vprintf(const char *fmt, va_list ap)
{
    char tmp[512];
    int  rc;
    size_t len;

    rc = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    len = strnlen(tmp, sizeof(tmp));
    if (len != 0U)
        (void)write(STDOUT_FILENO, tmp, len);
    return rc;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = vprintf(fmt, ap);
    va_end(ap);
    return rc;
}

int puts(const char *s)
{
    int rc = 0;

    if (s)
        rc = (int)write(STDOUT_FILENO, s, strlen(s));
    if (write(STDOUT_FILENO, "\n", 1U) < 0)
        return EOF;
    return rc;
}
