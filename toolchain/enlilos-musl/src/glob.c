#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int   flags;
    int (*errfunc)(const char *epath, int eerrno);
    int   matches_added;
} glob_ctx_t;

typedef struct {
    size_t path_capacity;
    size_t string_capacity;
    size_t string_used;
    char   data[];
} glob_store_t;

static int glob_is_dir_mode(mode_t mode)
{
    return (((unsigned int)mode & 0170000U) == 0040000U);
}

static size_t glob_strlen(const char *s)
{
    size_t n = 0U;

    while (s && s[n] != '\0')
        n++;
    return n;
}

static char *glob_strdup(const char *s)
{
    size_t len;
    char  *out;

    if (!s)
        return NULL;
    len = glob_strlen(s);
    out = (char *)malloc(len + 1U);
    if (!out)
        return NULL;
    (void)memcpy(out, s, len + 1U);
    return out;
}

static int glob_has_magic(const char *pattern)
{
    while (pattern && *pattern) {
        if (*pattern == '*' || *pattern == '?' || *pattern == '[')
            return 1;
        if (*pattern == '\\' && pattern[1] != '\0')
            pattern++;
        pattern++;
    }
    return 0;
}

static int glob_is_dot_entry(const char *name)
{
    return name &&
           (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static char *glob_join_path(const char *base, const char *name)
{
    size_t base_len = glob_strlen(base);
    size_t name_len = glob_strlen(name);
    size_t extra = 0U;
    char  *out;

    if (base_len == 0U)
        return glob_strdup(name);

    if (!(base_len == 1U && base[0] == '/'))
        extra = 1U;

    out = (char *)malloc(base_len + extra + name_len + 1U);
    if (!out)
        return NULL;

    (void)memcpy(out, base, base_len);
    if (extra)
        out[base_len++] = '/';
    (void)memcpy(out + base_len, name, name_len);
    out[base_len + name_len] = '\0';
    return out;
}

static int glob_path_exists(const char *path, int *is_dir)
{
    DIR *dir;
    int  fd;

    if (is_dir)
        *is_dir = 0;

    dir = opendir((path && path[0] != '\0') ? path : ".");
    if (dir) {
        if (is_dir)
            *is_dir = 1;
        (void)closedir(dir);
        return 1;
    }

    fd = open((path && path[0] != '\0') ? path : ".", O_RDONLY, 0);
    if (fd < 0)
        return 0;
    (void)close(fd);
    return 1;
}

static glob_store_t *glob_store_from_pathv(char **pathv)
{
    if (!pathv)
        return NULL;
    return (glob_store_t *)((unsigned char *)pathv - offsetof(glob_store_t, data));
}

static char **glob_store_pathv(glob_store_t *store)
{
    return (char **)store->data;
}

static char *glob_store_strings(glob_store_t *store, size_t offs)
{
    return (char *)(glob_store_pathv(store) + offs + 1U);
}

static int glob_store_reserve(glob_t *pglob, size_t want_pathc, size_t want_string_bytes)
{
    glob_store_t *old_store = glob_store_from_pathv(pglob->gl_pathv);
    size_t        old_path_capacity = old_store ? old_store->path_capacity : 0U;
    size_t        old_string_capacity = old_store ? old_store->string_capacity : 0U;
    size_t        old_string_used = old_store ? old_store->string_used : 0U;
    size_t        new_path_capacity = old_path_capacity;
    size_t        new_string_capacity = old_string_capacity;
    size_t        new_total_slots;
    size_t        new_bytes;
    glob_store_t *new_store;
    char        **new_pathv;
    char         *new_strings;

    if (new_path_capacity < want_pathc)
        new_path_capacity = (want_pathc < 8U) ? 8U : want_pathc;
    if (new_string_capacity < want_string_bytes)
        new_string_capacity = (want_string_bytes < 128U) ? 128U : want_string_bytes;

    while (new_path_capacity < want_pathc)
        new_path_capacity *= 2U;
    while (new_string_capacity < want_string_bytes)
        new_string_capacity *= 2U;

    if (old_store &&
        old_path_capacity >= want_pathc &&
        old_string_capacity >= want_string_bytes)
        return 0;

    new_total_slots = pglob->gl_offs + new_path_capacity + 1U;
    new_bytes = sizeof(*new_store) +
                new_total_slots * sizeof(char *) +
                new_string_capacity;

    new_store = (glob_store_t *)malloc(new_bytes);
    if (!new_store)
        return GLOB_NOSPACE;

    new_store->path_capacity = new_path_capacity;
    new_store->string_capacity = new_string_capacity;
    new_store->string_used = old_string_used;

    new_pathv = glob_store_pathv(new_store);
    new_strings = glob_store_strings(new_store, pglob->gl_offs + new_path_capacity);

    for (size_t i = 0U; i < new_total_slots; i++)
        new_pathv[i] = NULL;

    if (old_store) {
        char **old_pathv = glob_store_pathv(old_store);
        char  *old_strings = glob_store_strings(old_store, pglob->gl_offs + old_path_capacity);

        (void)memcpy(new_strings, old_strings, old_string_used);
        for (size_t i = 0U; i < pglob->gl_offs; i++)
            new_pathv[i] = NULL;
        for (size_t i = 0U; i < pglob->gl_pathc; i++) {
            size_t    slot = pglob->gl_offs + i;
            uintptr_t off  = (uintptr_t)old_pathv[slot] - (uintptr_t)old_strings;
            new_pathv[slot] = new_strings + off;
        }
        new_pathv[pglob->gl_offs + pglob->gl_pathc] = NULL;
        free(old_store);
    } else {
        for (size_t i = 0U; i < pglob->gl_offs; i++)
            new_pathv[i] = NULL;
        new_pathv[pglob->gl_offs] = NULL;
    }

    pglob->gl_pathv = new_pathv;
    return 0;
}

static int glob_add_match(glob_ctx_t *ctx, glob_t *pglob,
                          const char *path, int is_dir)
{
    glob_store_t *store;
    size_t        path_len;
    size_t        need_bytes;
    size_t        slot;
    char         *dst;
    int           rc;

    path_len = glob_strlen(path);
    need_bytes = path_len + 1U;
    if ((ctx->flags & GLOB_MARK) && is_dir && (path_len == 0U || path[path_len - 1U] != '/'))
        need_bytes++;

    rc = glob_store_reserve(pglob, pglob->gl_pathc + 1U,
                            glob_store_from_pathv(pglob->gl_pathv)
                                ? (glob_store_from_pathv(pglob->gl_pathv)->string_used + need_bytes)
                                : need_bytes);
    if (rc != 0)
        return rc;

    store = glob_store_from_pathv(pglob->gl_pathv);
    dst = glob_store_strings(store, pglob->gl_offs + store->path_capacity) + store->string_used;

    (void)memcpy(dst, path, path_len);
    if ((ctx->flags & GLOB_MARK) && is_dir && (path_len == 0U || path[path_len - 1U] != '/')) {
        dst[path_len++] = '/';
    }
    dst[path_len] = '\0';

    slot = pglob->gl_offs + pglob->gl_pathc;
    pglob->gl_pathv[slot] = dst;
    pglob->gl_pathc++;
    pglob->gl_pathv[pglob->gl_offs + pglob->gl_pathc] = NULL;
    store->string_used += path_len + 1U;
    ctx->matches_added++;
    return 0;
}

static int glob_sort_paths(glob_t *pglob)
{
    size_t start = pglob->gl_offs;
    size_t count = pglob->gl_pathc;

    for (size_t i = 1U; i < count; i++) {
        char  *key = pglob->gl_pathv[start + i];
        size_t j = i;

        while (j > 0U &&
               strcmp(pglob->gl_pathv[start + j - 1U], key) > 0) {
            pglob->gl_pathv[start + j] = pglob->gl_pathv[start + j - 1U];
            j--;
        }
        pglob->gl_pathv[start + j] = key;
    }
    return 0;
}

static int glob_parse_component(const char *pattern, char *segment, size_t segsz,
                                const char **rest_out)
{
    size_t n = 0U;

    while (*pattern == '/')
        pattern++;

    while (*pattern != '\0' && *pattern != '/') {
        if (n + 1U >= segsz)
            return GLOB_ABORTED;
        segment[n++] = *pattern++;
    }
    segment[n] = '\0';
    while (*pattern == '/')
        pattern++;
    *rest_out = pattern;
    return 0;
}

static int glob_handle_error(glob_ctx_t *ctx, const char *path)
{
    int e = errno;

    if (ctx->errfunc && ctx->errfunc(path, e) != 0)
        return GLOB_ABORTED;
    if (ctx->flags & GLOB_ERR)
        return GLOB_ABORTED;
    return 0;
}

static int glob_expand(glob_ctx_t *ctx, glob_t *pglob,
                       const char *base, const char *pattern)
{
    char        segment[64];
    const char *rest = pattern;
    const char *segment_pattern;
    int         rc;

    while (*rest == '/')
        rest++;
    segment_pattern = rest;

    if (*rest == '\0') {
        int is_dir = 0;

        if (!glob_path_exists((base && base[0] != '\0') ? base : ".", &is_dir))
            return 0;
        return glob_add_match(ctx, pglob,
                              (base && base[0] != '\0') ? base : ".", is_dir);
    }

    rc = glob_parse_component(rest, segment, sizeof(segment), &rest);
    if (rc != 0)
        return rc;

    if (strcmp(segment, "**") == 0) {
        DIR           *dir;
        struct dirent *ent;

        rc = glob_expand(ctx, pglob, base, rest);
        if (rc != 0)
            return rc;

        dir = opendir((base && base[0] != '\0') ? base : ".");
        if (!dir)
            return glob_handle_error(ctx, (base && base[0] != '\0') ? base : ".");

        while ((ent = readdir(dir)) != NULL) {
            char *child;

            if (glob_is_dot_entry(ent->d_name))
                continue;
            if (ent->d_name[0] == '.')
                continue;
            if (!glob_is_dir_mode(ent->d_mode))
                continue;

            child = glob_join_path(base, ent->d_name);
            if (!child) {
                (void)closedir(dir);
                return GLOB_NOSPACE;
            }

            rc = glob_expand(ctx, pglob, child, segment_pattern);
            free(child);
            if (rc != 0) {
                (void)closedir(dir);
                return rc;
            }
        }

        (void)closedir(dir);
        return 0;
    }

    if (!glob_has_magic(segment)) {
        char *child = glob_join_path(base, segment);

        if (!child)
            return GLOB_NOSPACE;

        if (*rest != '\0') {
            int is_dir = 0;

            if (glob_path_exists(child, &is_dir) && is_dir)
                rc = glob_expand(ctx, pglob, child, rest);
            else
                rc = 0;
        } else {
            int is_dir = 0;

            rc = glob_path_exists(child, &is_dir) ?
                 glob_add_match(ctx, pglob, child, is_dir) : 0;
        }

        free(child);
        return rc;
    }

    {
        DIR           *dir;
        struct dirent *ent;
        int            fnm_flags = FNM_PERIOD |
                                   ((ctx->flags & GLOB_NOESCAPE) ? FNM_NOESCAPE : 0);

        dir = opendir((base && base[0] != '\0') ? base : ".");
        if (!dir)
            return glob_handle_error(ctx, (base && base[0] != '\0') ? base : ".");

        while ((ent = readdir(dir)) != NULL) {
            char *child;

            if (glob_is_dot_entry(ent->d_name))
                continue;
            if (fnmatch(segment, ent->d_name, fnm_flags) != 0)
                continue;

            child = glob_join_path(base, ent->d_name);
            if (!child) {
                (void)closedir(dir);
                return GLOB_NOSPACE;
            }

            if (*rest != '\0') {
                if (glob_is_dir_mode(ent->d_mode))
                    rc = glob_expand(ctx, pglob, child, rest);
                else
                    rc = 0;
            } else {
                rc = glob_add_match(ctx, pglob, child,
                                    glob_is_dir_mode(ent->d_mode));
            }

            free(child);
            if (rc != 0) {
                (void)closedir(dir);
                return rc;
            }
        }

        (void)closedir(dir);
    }

    return 0;
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *epath, int eerrno),
         glob_t *pglob)
{
    glob_ctx_t ctx;
    char      *base;
    const char *orig_pattern = pattern;
    int        rc;

    if (!pattern || !pglob)
        return GLOB_ABORTED;

    if (!(flags & GLOB_APPEND)) {
        globfree(pglob);
        pglob->gl_pathc = 0U;
        pglob->gl_pathv = NULL;
        if (!(flags & GLOB_DOOFFS))
            pglob->gl_offs = 0U;
    }

    ctx.flags = flags;
    ctx.errfunc = errfunc;
    ctx.matches_added = 0;

    if (pattern[0] == '/') {
        base = glob_strdup("/");
        while (*pattern == '/')
            pattern++;
    } else {
        base = glob_strdup("");
    }

    if (!base)
        return GLOB_NOSPACE;

    rc = glob_expand(&ctx, pglob, base, pattern);
    free(base);
    if (rc != 0)
        return rc;

    if (ctx.matches_added == 0) {
        if (flags & GLOB_NOCHECK)
            return glob_add_match(&ctx, pglob,
                                  (orig_pattern && orig_pattern[0] != '\0') ? orig_pattern : ".",
                                  0);
        return GLOB_NOMATCH;
    }

    if (!(flags & GLOB_NOSORT))
        glob_sort_paths(pglob);
    return 0;
}

void globfree(glob_t *pglob)
{
    glob_store_t *store;

    if (!pglob || !pglob->gl_pathv)
        return;

    store = glob_store_from_pathv(pglob->gl_pathv);
    free(store);
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0U;
}
