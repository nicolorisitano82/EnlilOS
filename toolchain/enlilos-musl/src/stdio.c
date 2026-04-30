#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct __enlilos_file {
    int            fd;
    unsigned int   flags;
    unsigned int   is_static;
};

enum {
    FILE_FLAG_READ   = 1U << 0,
    FILE_FLAG_WRITE  = 1U << 1,
    FILE_FLAG_APPEND = 1U << 2,
    FILE_FLAG_EOF    = 1U << 3,
    FILE_FLAG_ERR    = 1U << 4
};

static FILE g_stdin  = { STDIN_FILENO,  FILE_FLAG_READ,  1U };
static FILE g_stdout = { STDOUT_FILENO, FILE_FLAG_WRITE, 1U };
static FILE g_stderr = { STDERR_FILENO, FILE_FLAG_WRITE, 1U };

FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

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

        /* Handle length modifiers: 'l' (long), 'z' (size_t), 'j' (intmax_t) */
        while (*fmt == 'l') {
            long_flag++;
            fmt++;
        }
        if (*fmt == 'z' || *fmt == 'j') {
            long_flag = 2; /* treat as unsigned long / long for 64-bit targets */
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

static FILE *stdio_alloc_stream(int fd, unsigned int flags)
{
    FILE *stream = (FILE *)malloc(sizeof(*stream));

    if (!stream)
        return NULL;
    stream->fd = fd;
    stream->flags = flags;
    stream->is_static = 0U;
    return stream;
}

static int stdio_parse_mode(const char *mode, int *open_flags, unsigned int *file_flags)
{
    int plus = 0;

    if (!mode || !open_flags || !file_flags)
        return -1;

    while (*mode == 'b')
        mode++;

    switch (*mode) {
    case 'r':
        *open_flags = O_RDONLY;
        *file_flags = FILE_FLAG_READ;
        break;
    case 'w':
        *open_flags = O_WRONLY | O_CREAT | O_TRUNC;
        *file_flags = FILE_FLAG_WRITE;
        break;
    case 'a':
        *open_flags = O_WRONLY | O_CREAT | O_APPEND;
        *file_flags = FILE_FLAG_WRITE | FILE_FLAG_APPEND;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    mode++;
    while (*mode != '\0') {
        if (*mode == '+')
            plus = 1;
        else if (*mode != 'b') {
            errno = EINVAL;
            return -1;
        }
        mode++;
    }

    if (plus) {
        *open_flags &= ~(O_RDONLY | O_WRONLY);
        *open_flags |= O_RDWR;
        *file_flags |= FILE_FLAG_READ | FILE_FLAG_WRITE;
    }
    return 0;
}

static int stdio_stream_write(FILE *stream, const void *buf, size_t len)
{
    const char *cursor = (const char *)buf;

    while (len > 0U) {
        ssize_t rc = write(stream->fd, cursor, len);

        if (rc < 0) {
            stream->flags |= FILE_FLAG_ERR;
            return EOF;
        }
        cursor += (size_t)rc;
        len -= (size_t)rc;
    }

    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    int          fd;
    int          open_flags;
    unsigned int file_flags;

    if (stdio_parse_mode(mode, &open_flags, &file_flags) < 0)
        return NULL;

    fd = open(path, open_flags, 0666);
    if (fd < 0)
        return NULL;

    return stdio_alloc_stream(fd, file_flags);
}

FILE *fdopen(int fd, const char *mode)
{
    int          open_flags = 0;
    unsigned int file_flags = 0U;

    (void)open_flags;
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    if (stdio_parse_mode(mode, &open_flags, &file_flags) < 0)
        return NULL;
    return stdio_alloc_stream(fd, file_flags);
}

FILE *tmpfile(void)
{
    static unsigned int counter = 0U;
    static const char  *prefixes[] = { "/data/tmp", "/data", "/tmp", "." };
    char                path[96];
    unsigned int        id = counter++;

    for (size_t i = 0U; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        FILE *stream;

        (void)snprintf(path, sizeof(path), "%s/enlil-tmp-%d-%u.tmp",
                       prefixes[i], (int)getpid(), id);
        stream = fopen(path, "w+");
        if (stream)
            return stream;
    }

    errno = EIO;
    return NULL;
}

int fclose(FILE *stream)
{
    int rc;

    if (!stream) {
        errno = EINVAL;
        return EOF;
    }

    rc = close(stream->fd);
    if (!stream->is_static)
        free(stream);
    return rc < 0 ? EOF : 0;
}

int fflush(FILE *stream)
{
    (void)stream;
    return 0;
}

int fileno(FILE *stream)
{
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    return stream->fd;
}

int ferror(FILE *stream)
{
    return (stream && (stream->flags & FILE_FLAG_ERR) != 0U) ? 1 : 0;
}

int feof(FILE *stream)
{
    return (stream && (stream->flags & FILE_FLAG_EOF) != 0U) ? 1 : 0;
}

void clearerr(FILE *stream)
{
    if (!stream)
        return;
    stream->flags &= ~(FILE_FLAG_EOF | FILE_FLAG_ERR);
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
    (void)stream;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

void setlinebuf(FILE *stream)
{
    (void)setvbuf(stream, NULL, _IOLBF, 0U);
}

void rewind(FILE *stream)
{
    if (!stream)
        return;
    clearerr(stream);
    (void)lseek(stream->fd, 0, SEEK_SET);
}

int fgetc(FILE *stream)
{
    unsigned char ch;
    ssize_t       rc;

    if (!stream || (stream->flags & FILE_FLAG_READ) == 0U) {
        errno = EINVAL;
        if (stream)
            stream->flags |= FILE_FLAG_ERR;
        return EOF;
    }

    rc = read(stream->fd, &ch, 1U);
    if (rc == 0) {
        stream->flags |= FILE_FLAG_EOF;
        return EOF;
    }
    if (rc < 0) {
        stream->flags |= FILE_FLAG_ERR;
        return EOF;
    }
    return (int)ch;
}

int fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;

    if (!stream || (stream->flags & FILE_FLAG_WRITE) == 0U) {
        errno = EINVAL;
        if (stream)
            stream->flags |= FILE_FLAG_ERR;
        return EOF;
    }
    if (stdio_stream_write(stream, &ch, 1U) < 0)
        return EOF;
    return (int)ch;
}

int fputs(const char *s, FILE *stream)
{
    size_t len;

    if (!s)
        s = "(null)";
    len = strlen(s);
    if (stdio_stream_write(stream, s, len) < 0)
        return EOF;
    return (int)len;
}

char *fgets(char *s, int size, FILE *stream)
{
    int i = 0;

    if (!s || size <= 0 || !stream) {
        errno = EINVAL;
        return NULL;
    }

    while (i + 1 < size) {
        int ch = fgetc(stream);

        if (ch == EOF)
            break;
        s[i++] = (char)ch;
        if (ch == '\n')
            break;
    }

    if (i == 0)
        return NULL;
    s[i] = '\0';
    return s;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    unsigned char *out = (unsigned char *)ptr;
    size_t         total = size * nmemb;
    size_t         done = 0U;

    if (size == 0U || nmemb == 0U)
        return 0U;

    while (done < total) {
        ssize_t rc = read(stream->fd, out + done, total - done);

        if (rc == 0) {
            stream->flags |= FILE_FLAG_EOF;
            break;
        }
        if (rc < 0) {
            stream->flags |= FILE_FLAG_ERR;
            break;
        }
        done += (size_t)rc;
    }

    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    const unsigned char *in = (const unsigned char *)ptr;
    size_t               total = size * nmemb;
    size_t               done = 0U;

    if (size == 0U || nmemb == 0U)
        return 0U;

    while (done < total) {
        ssize_t rc = write(stream->fd, in + done, total - done);

        if (rc <= 0) {
            stream->flags |= FILE_FLAG_ERR;
            break;
        }
        done += (size_t)rc;
    }

    return done / size;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    va_list ap_copy;
    int     need;
    char   *buf;
    int     rc;

    va_copy(ap_copy, ap);
    need = vsnprintf(NULL, 0U, fmt, ap_copy);
    va_end(ap_copy);
    if (need < 0)
        return EOF;

    buf = (char *)malloc((size_t)need + 1U);
    if (!buf) {
        errno = ENOMEM;
        return EOF;
    }

    rc = vsnprintf(buf, (size_t)need + 1U, fmt, ap);
    if (stdio_stream_write(stream, buf, (size_t)need) < 0)
        rc = EOF;
    free(buf);
    return rc;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = vfprintf(stream, fmt, ap);
    va_end(ap);
    return rc;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return rc;
}

int perror(const char *s)
{
    if (s && *s)
        (void)fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        (void)fprintf(stderr, "%s\n", strerror(errno));
    return 0;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list     ap;
    const char *in = str;
    int         assigned = 0;

    va_start(ap, fmt);
    while (*fmt != '\0') {
        int width = 0;

        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt))
                fmt++;
            while (isspace((unsigned char)*in))
                in++;
            continue;
        }

        if (*fmt != '%') {
            if (*in != *fmt)
                break;
            in++;
            fmt++;
            continue;
        }

        fmt++;
        while (isdigit((unsigned char)*fmt)) {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (width <= 0)
            width = 1024;

        switch (*fmt) {
        case 's': {
            char *out = va_arg(ap, char *);
            int   n = 0;

            while (isspace((unsigned char)*in))
                in++;
            if (*in == '\0')
                goto sscanf_done;

            while (*in != '\0' && !isspace((unsigned char)*in) && n + 1 < width)
                out[n++] = *in++;
            out[n] = '\0';
            assigned++;
            fmt++;
            break;
        }
        case 'l':
            fmt++;
            if (*fmt == 'u') {
                unsigned long *out = va_arg(ap, unsigned long *);
                char          *endptr;
                unsigned long  value;

                while (isspace((unsigned char)*in))
                    in++;
                value = strtoul(in, &endptr, 10);
                if (endptr == in)
                    goto sscanf_done;
                *out = value;
                in = endptr;
                assigned++;
                fmt++;
                break;
            }
            goto sscanf_done;
        default:
            goto sscanf_done;
        }
    }

sscanf_done:
    va_end(ap);
    return assigned;
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
    return vfprintf(stdout, fmt, ap);
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
    int rc;

    rc = fputs(s ? s : "(null)", stdout);
    if (fputc('\n', stdout) == EOF)
        return EOF;
    return rc;
}

int putc(int c, FILE *stream)
{
    return fputc(c, stream);
}

int putchar(int c)
{
    return fputc(c, stdout);
}
