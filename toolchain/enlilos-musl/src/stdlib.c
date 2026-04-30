#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

extern char **environ;

#define ENLIL_LONG_MAX      ((long)(~0UL >> 1))
#define ENLIL_LONG_MIN      (-ENLIL_LONG_MAX - 1L)
#define ENLIL_LLONG_MAX     ((long long)(~0ULL >> 1))
#define ENLIL_LLONG_MIN     (-ENLIL_LLONG_MAX - 1LL)

static const char *skip_space(const char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    return s;
}

static int digit_value(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 10;
    return -1;
}

static unsigned long long parse_unsigned_core(const char **cursor, int base, int *any)
{
    unsigned long long value = 0ULL;
    int                overflow = 0;
    const char        *s = *cursor;

    *any = 0;
    while (*s != '\0') {
        int digit = digit_value((unsigned char)*s);

        if (digit < 0 || digit >= base)
            break;
        *any = 1;
        if (value > (~0ULL - (unsigned long long)digit) / (unsigned long long)base) {
            overflow = 1;
            value = ~0ULL;
        } else if (!overflow) {
            value = value * (unsigned long long)base + (unsigned long long)digit;
        }
        s++;
    }

    if (overflow)
        errno = ERANGE;
    *cursor = s;
    return value;
}

static unsigned long long parse_integer_prefix(const char **cursor, int *base)
{
    const char *s = *cursor;

    if (*base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            *base = 16;
            s += 2;
        } else if (s[0] == '0') {
            *base = 8;
            s++;
        } else {
            *base = 10;
        }
    } else if (*base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    if (*base < 2 || *base > 36) {
        errno = EINVAL;
        return 0ULL;
    }

    *cursor = s;
    return 0ULL;
}

static unsigned long long parse_unsigned_integer(const char *nptr, char **endptr, int base)
{
    const char        *s = skip_space(nptr);
    const char        *digits;
    unsigned long long value;
    int                negative = 0;
    int                any = 0;

    if (*s == '+' || *s == '-') {
        negative = (*s == '-');
        s++;
    }

    digits = s;
    (void)parse_integer_prefix(&digits, &base);
    if (errno == EINVAL) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0ULL;
    }

    value = parse_unsigned_core(&digits, base, &any);
    if (!any) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0ULL;
    }

    if (endptr)
        *endptr = (char *)digits;
    return negative ? (unsigned long long)(-(long long)value) : value;
}

static long long parse_signed_integer(const char *nptr, char **endptr, int base, int long_long_mode)
{
    const char        *s = skip_space(nptr);
    const char        *digits;
    unsigned long long value;
    unsigned long long limit;
    int                negative = 0;
    int                any = 0;
    long long          result;

    if (*s == '+' || *s == '-') {
        negative = (*s == '-');
        s++;
    }

    digits = s;
    (void)parse_integer_prefix(&digits, &base);
    if (errno == EINVAL) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0;
    }

    value = parse_unsigned_core(&digits, base, &any);
    if (!any) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0;
    }

    if (endptr)
        *endptr = (char *)digits;

    if (long_long_mode) {
        limit = negative ? ((~0ULL >> 1) + 1ULL) : (~0ULL >> 1);
    } else {
        unsigned long max_long = ~0UL >> 1;
        limit = negative ? ((unsigned long long)max_long + 1ULL)
                         : (unsigned long long)max_long;
    }

    if (value > limit) {
        errno = ERANGE;
        if (long_long_mode)
            return negative ? ENLIL_LLONG_MIN : ENLIL_LLONG_MAX;
        return negative ? ENLIL_LONG_MIN : ENLIL_LONG_MAX;
    }

    if (negative) {
        if (value == ((unsigned long long)ENLIL_LLONG_MAX + 1ULL))
            result = ENLIL_LLONG_MIN;
        else
            result = -(long long)value;
    } else {
        result = (long long)value;
    }

    return long_long_mode ? result : (long)result;
}

static double apply_base10_exp(double value, int exp)
{
    double scale = 10.0;

    if (exp < 0) {
        exp = -exp;
        scale = 0.1;
    }

    while (exp > 0) {
        value *= scale;
        exp--;
    }
    return value;
}

static size_t env_count(void)
{
    size_t count = 0U;

    if (!environ)
        return 0U;
    while (environ[count] != NULL)
        count++;
    return count;
}

static ssize_t env_find(const char *name)
{
    size_t name_len;
    size_t i;

    if (!name || !environ)
        return -1;
    name_len = strlen(name);
    for (i = 0; environ[i] != NULL; i++) {
        if (strncmp(environ[i], name, name_len) == 0 && environ[i][name_len] == '=')
            return (ssize_t)i;
    }
    return -1;
}

static char *env_make_entry(const char *name, const char *value)
{
    size_t name_len;
    size_t value_len;
    char  *entry;

    name_len = strlen(name);
    value_len = strlen(value);
    entry = (char *)malloc(name_len + value_len + 2U);
    if (!entry)
        return NULL;

    (void)memcpy(entry, name, name_len);
    entry[name_len] = '=';
    (void)memcpy(entry + name_len + 1U, value, value_len + 1U);
    return entry;
}

static unsigned long g_rand_state = 1UL;
static unsigned long g_tmp_counter = 0UL;

static unsigned long enlil_rand_next(void)
{
    g_rand_state = g_rand_state * 1103515245UL + 12345UL;
    return g_rand_state;
}

static int tmp_template_tail(char *template_name)
{
    size_t len;

    if (!template_name) {
        errno = EINVAL;
        return -1;
    }

    len = strlen(template_name);
    if (len < 6U || strcmp(template_name + len - 6U, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }

    return (int)(len - 6U);
}

static void tmp_fill_suffix(char *tail)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned long value = enlil_rand_next() ^ (unsigned long)getpid() ^ g_tmp_counter++;

    for (int i = 0; i < 6; i++) {
        tail[i] = alphabet[value % (sizeof(alphabet) - 1U)];
        value /= (sizeof(alphabet) - 1U);
        if (value == 0UL)
            value = enlil_rand_next() ^ ((unsigned long)(i + 1) << 8);
    }
}

int atoi(const char *nptr)
{
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr)
{
    return strtol(nptr, NULL, 10);
}

double atof(const char *nptr)
{
    return strtod(nptr, NULL);
}

int rand(void)
{
    return (int)(enlil_rand_next() & 0x7fffffffUL);
}

void srand(unsigned int seed)
{
    g_rand_state = (unsigned long)seed;
    if (g_rand_state == 0UL)
        g_rand_state = 1UL;
}

long strtol(const char *nptr, char **endptr, int base)
{
    return (long)parse_signed_integer(nptr, endptr, base, 0);
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    return (unsigned long)parse_unsigned_integer(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base)
{
    return parse_signed_integer(nptr, endptr, base, 1);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    return parse_unsigned_integer(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr)
{
    const char *s = skip_space(nptr);
    const char *start = s;
    int         negative = 0;
    double      value = 0.0;
    double      frac_scale = 0.1;
    int         any = 0;

    if (*s == '+' || *s == '-') {
        negative = (*s == '-');
        s++;
    }

    while (isdigit((unsigned char)*s)) {
        value = value * 10.0 + (double)(*s - '0');
        any = 1;
        s++;
    }

    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            value += (double)(*s - '0') * frac_scale;
            frac_scale *= 0.1;
            any = 1;
            s++;
        }
    }

    if (!any) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0.0;
    }

    if (*s == 'e' || *s == 'E') {
        const char *exp_ptr = s + 1;
        int         exp_neg = 0;
        int         exp_any = 0;
        int         exp = 0;

        if (*exp_ptr == '+' || *exp_ptr == '-') {
            exp_neg = (*exp_ptr == '-');
            exp_ptr++;
        }
        while (isdigit((unsigned char)*exp_ptr)) {
            exp = exp * 10 + (*exp_ptr - '0');
            exp_any = 1;
            exp_ptr++;
        }
        if (exp_any) {
            if (exp_neg)
                exp = -exp;
            value = apply_base10_exp(value, exp);
            s = exp_ptr;
        }
    }

    if (endptr)
        *endptr = (char *)s;

    if (negative)
        value = -value;
    (void)start;
    return value;
}

char *mktemp(char *template_name)
{
    int offset;

    offset = tmp_template_tail(template_name);
    if (offset < 0)
        return NULL;

    for (int attempt = 0; attempt < 256; attempt++) {
        struct stat st;

        tmp_fill_suffix(template_name + offset);
        if (lstat(template_name, &st) < 0) {
            if (errno == ENOENT)
                return template_name;
            return NULL;
        }
    }

    errno = EEXIST;
    if (offset >= 0)
        memcpy(template_name + offset, "XXXXXX", 6U);
    return NULL;
}

int mkstemp(char *template_name)
{
    int offset;

    offset = tmp_template_tail(template_name);
    if (offset < 0)
        return -1;

    for (int attempt = 0; attempt < 256; attempt++) {
        int fd;

        tmp_fill_suffix(template_name + offset);
        fd = open(template_name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }

    errno = EEXIST;
    memcpy(template_name + offset, "XXXXXX", 6U);
    return -1;
}

char *mkdtemp(char *template_name)
{
    int offset;

    offset = tmp_template_tail(template_name);
    if (offset < 0)
        return NULL;

    for (int attempt = 0; attempt < 256; attempt++) {
        tmp_fill_suffix(template_name + offset);
        if (mkdir(template_name, 0700) == 0)
            return template_name;
        if (errno != EEXIST)
            return NULL;
    }

    errno = EEXIST;
    memcpy(template_name + offset, "XXXXXX", 6U);
    return NULL;
}

char *getenv(const char *name)
{
    ssize_t idx;
    size_t  name_len;

    if (!name || name[0] == '\0')
        return NULL;

    idx = env_find(name);
    if (idx < 0)
        return NULL;

    name_len = strlen(name);
    return environ[idx] + name_len + 1U;
}

int setenv(const char *name, const char *value, int overwrite)
{
    ssize_t idx;
    char   *entry;

    if (!name || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!value)
        value = "";

    idx = env_find(name);
    if (idx >= 0 && !overwrite)
        return 0;

    entry = env_make_entry(name, value);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }

    if (idx >= 0) {
        environ[idx] = entry;
        return 0;
    }

    {
        size_t count = env_count();
        char **new_env = (char **)malloc(sizeof(char *) * (count + 2U));

        if (!new_env) {
            errno = ENOMEM;
            return -1;
        }

        for (size_t i = 0; i < count; i++)
            new_env[i] = environ[i];
        new_env[count] = entry;
        new_env[count + 1U] = NULL;
        environ = new_env;
    }

    return 0;
}

int unsetenv(const char *name)
{
    ssize_t idx;
    size_t  count;

    if (!name || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    idx = env_find(name);
    if (idx < 0)
        return 0;

    count = env_count();
    for (size_t i = (size_t)idx; i + 1U < count; i++)
        environ[i] = environ[i + 1U];
    environ[count - 1U] = NULL;
    return 0;
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    unsigned char *arr = (unsigned char *)base;
    void          *tmp;

    if (!base || nmemb < 2U || size == 0U || !compar)
        return;

    tmp = malloc(size);
    if (!tmp)
        return;

    for (size_t i = 1U; i < nmemb; i++) {
        size_t j = i;

        (void)memcpy(tmp, arr + (i * size), size);
        while (j > 0U && compar(arr + ((j - 1U) * size), tmp) > 0) {
            (void)memmove(arr + (j * size), arr + ((j - 1U) * size), size);
            j--;
        }
        (void)memcpy(arr + (j * size), tmp, size);
    }

    free(tmp);
}
