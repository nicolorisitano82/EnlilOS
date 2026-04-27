#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include <toon.h>

static void toon_reset(toon_doc_t *doc)
{
    if (!doc)
        return;
    memset(doc, 0, sizeof(*doc));
}

static void toon_set_error(toon_doc_t *doc, const char *msg)
{
    if (!doc)
        return;
    if (!msg)
        msg = "toon parse error";
    strncpy(doc->error, msg, sizeof(doc->error) - 1U);
    doc->error[sizeof(doc->error) - 1U] = '\0';
}

static char *toon_skip_spaces(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static char *toon_rtrim(char *s)
{
    size_t len;

    if (!s)
        return s;
    len = strlen(s);
    while (len > 0U) {
        char ch = s[len - 1U];

        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            break;
        s[--len] = '\0';
    }
    return s;
}

static int toon_unescape_inline(char *dst, size_t cap, const char *src)
{
    size_t pos = 0U;

    if (!dst || !src || cap == 0U)
        return -1;

    while (*src != '\0') {
        char ch = *src++;

        if (ch == '\\') {
            char esc = *src++;

            switch (esc) {
            case '\\': ch = '\\'; break;
            case '"':  ch = '"';  break;
            case 'n':  ch = '\n'; break;
            case 'r':  ch = '\r'; break;
            case 't':  ch = '\t'; break;
            default:
                return -1;
            }
        }
        if (pos + 1U >= cap)
            return -1;
        dst[pos++] = ch;
    }

    dst[pos] = '\0';
    return 0;
}

static int toon_parse_value(char *value)
{
    size_t len;

    if (!value)
        return -1;
    value = toon_skip_spaces(value);
    toon_rtrim(value);
    len = strlen(value);
    if (len >= 2U && value[0] == '"' && value[len - 1U] == '"') {
        value[len - 1U] = '\0';
        if (toon_unescape_inline(value, len, value + 1) < 0)
            return -1;
    }
    return 0;
}

static int toon_parse_array_count(const char *count_str)
{
    int count = 0;

    if (!count_str || *count_str == '\0')
        return -1;
    while (*count_str != '\0') {
        if (*count_str < '0' || *count_str > '9')
            return -1;
        count = count * 10 + (*count_str - '0');
        count_str++;
    }
    return count;
}

static int toon_add_scalar(toon_doc_t *doc, char *key, char *value)
{
    if (doc->nscalars >= (int)TOON_MAX_SCALARS) {
        toon_set_error(doc, "troppi scalari");
        return -1;
    }
    doc->scalars[doc->nscalars].key = key;
    doc->scalars[doc->nscalars].value = value;
    doc->nscalars++;
    return 0;
}

static int toon_add_array(toon_doc_t *doc, char *key, char *value, int expected_count)
{
    toon_arr_t *arr;
    int         count = 0;
    char       *cursor;

    if (doc->narrays >= (int)TOON_MAX_ARRAYS) {
        toon_set_error(doc, "troppi array");
        return -1;
    }

    arr = &doc->arrays[doc->narrays];
    arr->key = key;
    arr->count = 0;

    cursor = value;
    while (*cursor != '\0') {
        char *item;
        char *comma;

        if (count >= (int)TOON_MAX_ARRAY_ITEMS) {
            toon_set_error(doc, "troppi item in array");
            return -1;
        }

        item = toon_skip_spaces(cursor);
        comma = item;
        while (*comma != '\0' && *comma != ',')
            comma++;
        if (*comma == ',') {
            *comma = '\0';
            cursor = comma + 1;
        } else {
            cursor = comma;
        }

        toon_rtrim(item);
        if (toon_parse_value(item) < 0) {
            toon_set_error(doc, "valore array non valido");
            return -1;
        }
        arr->items[count++] = item;
        if (*cursor == '\0')
            break;
    }

    if (count != expected_count) {
        toon_set_error(doc, "count array mismatch");
        return -1;
    }

    arr->count = count;
    doc->narrays++;
    return 0;
}

int toon_parse(const char *src, size_t len, toon_doc_t *out)
{
    char  *cursor;
    size_t copy_len;

    if (!src || !out) {
        errno = EINVAL;
        return -1;
    }

    toon_reset(out);
    copy_len = len;
    if (copy_len >= sizeof(out->storage)) {
        toon_set_error(out, "manifest troppo grande");
        errno = ENOMEM;
        return -1;
    }

    memcpy(out->storage, src, copy_len);
    out->storage[copy_len] = '\0';
    out->storage_len = copy_len;

    cursor = out->storage;
    while (*cursor != '\0') {
        char *line = cursor;
        char *colon;
        char *key_end;
        char *value;

        while (*cursor != '\0' && *cursor != '\n')
            cursor++;
        if (*cursor == '\n')
            *cursor++ = '\0';

        line = toon_skip_spaces(line);
        toon_rtrim(line);
        if (*line == '\0')
            continue;

        colon = strchr(line, ':');
        if (!colon) {
            toon_set_error(out, "colon mancante");
            errno = EINVAL;
            return -1;
        }
        *colon = '\0';
        key_end = toon_rtrim(line);
        value = toon_skip_spaces(colon + 1);

        {
            char *obr = strchr(line, '[');
            char *cbr = strchr(line, ']');

            if (obr || cbr) {
                int expected_count;

                if (!obr || !cbr || cbr < obr) {
                    toon_set_error(out, "array header non valido");
                    errno = EINVAL;
                    return -1;
                }
                *obr = '\0';
                *cbr = '\0';
                key_end = toon_rtrim(line);
                expected_count = toon_parse_array_count(obr + 1);
                if (expected_count < 0 ||
                    toon_add_array(out, key_end, value, expected_count) < 0) {
                    if (out->error[0] == '\0')
                        toon_set_error(out, "array non valido");
                    errno = EINVAL;
                    return -1;
                }
                continue;
            }
        }

        if (toon_parse_value(value) < 0 || toon_add_scalar(out, key_end, value) < 0) {
            if (out->error[0] == '\0')
                toon_set_error(out, "valore non valido");
            errno = EINVAL;
            return -1;
        }
    }

    return 0;
}

int toon_parse_file(const char *path, toon_doc_t *out)
{
    int     fd;
    ssize_t n;
    char    buf[TOON_MAX_STORAGE];

    if (!path || !out) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    n = read(fd, buf, sizeof(buf) - 1U);
    if (n < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    buf[(size_t)n] = '\0';
    close(fd);
    return toon_parse(buf, (size_t)n, out);
}

const char *toon_get_string(const toon_doc_t *doc, const char *key)
{
    int i;

    if (!doc || !key)
        return NULL;
    for (i = 0; i < doc->nscalars; i++) {
        if (strcmp(doc->scalars[i].key, key) == 0)
            return doc->scalars[i].value;
    }
    return NULL;
}

int toon_get_bool(const toon_doc_t *doc, const char *key, int def)
{
    const char *value = toon_get_string(doc, key);

    if (!value)
        return def;
    if (strcmp(value, "true") == 0)
        return 1;
    if (strcmp(value, "false") == 0)
        return 0;
    return def;
}

const char **toon_get_array(const toon_doc_t *doc, const char *key, int *count_out)
{
    int i;

    if (!doc || !key)
        return NULL;
    for (i = 0; i < doc->narrays; i++) {
        if (strcmp(doc->arrays[i].key, key) == 0) {
            if (count_out)
                *count_out = doc->arrays[i].count;
            return doc->arrays[i].items;
        }
    }
    return NULL;
}
