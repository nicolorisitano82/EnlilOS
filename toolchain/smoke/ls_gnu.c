#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int show_all;
    int long_format;
    int human;
    int classify;
    int recursive;
    int directory_as_file;
} ls_opts_t;

typedef struct {
    char        *name;
    char        *full_path;
    struct stat  st;
    int          stat_ok;
} ls_entry_t;

static void ls_usage(FILE *out)
{
    fprintf(out,
            "usage: ls [OPTION]... [FILE]...\n"
            "EnlilOS ls (GNU-like v1)\n\n"
            "  -a          mostra anche i file nascosti\n"
            "  -l          formato lungo\n"
            "  -h          dimensioni leggibili (con -l)\n"
            "  -F          aggiunge un indicatore al nome (/ * @)\n"
            "  -R          ricorsivo\n"
            "  -d          lista la directory come file\n"
            "  -1          una voce per riga (default)\n"
            "  --help      mostra questo aiuto\n"
            "  --version   mostra la versione\n");
}

static void ls_version(void)
{
    puts("ls (EnlilOS GNU-like v1)");
}

static int ls_cmp_entry(const void *lhs, const void *rhs)
{
    const ls_entry_t *a = (const ls_entry_t *)lhs;
    const ls_entry_t *b = (const ls_entry_t *)rhs;
    return strcmp(a->name, b->name);
}

static char *ls_strdup_range(const char *src)
{
    size_t len;
    char  *copy;

    if (!src)
        return NULL;
    len = strlen(src);
    copy = (char *)malloc(len + 1U);
    if (!copy)
        return NULL;
    memcpy(copy, src, len + 1U);
    return copy;
}

static char *ls_join_path(const char *dir, const char *name)
{
    size_t dir_len;
    size_t name_len;
    int    need_slash;
    char  *buf;

    if (!dir || !name)
        return NULL;

    dir_len = strlen(dir);
    name_len = strlen(name);
    need_slash = (dir_len != 0U && dir[dir_len - 1U] != '/') ? 1 : 0;

    buf = (char *)malloc(dir_len + (size_t)need_slash + name_len + 1U);
    if (!buf)
        return NULL;

    memcpy(buf, dir, dir_len);
    if (need_slash)
        buf[dir_len++] = '/';
    memcpy(buf + dir_len, name, name_len + 1U);
    return buf;
}

static void ls_mode_string(mode_t mode, char out[11])
{
    out[0] = S_ISDIR(mode)  ? 'd' :
             S_ISLNK(mode)  ? 'l' :
             S_ISCHR(mode)  ? 'c' :
             S_ISBLK(mode)  ? 'b' :
             S_ISFIFO(mode) ? 'p' :
             S_ISSOCK(mode) ? 's' : '-';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void ls_format_size(off_t size, int human, char *buf, size_t buf_size)
{
    static const char units[] = { 'B', 'K', 'M', 'G', 'T' };
    unsigned long     value = (unsigned long)((size < 0) ? 0 : size);
    unsigned long     rem = 0UL;
    size_t            unit = 0U;

    if (!human) {
        snprintf(buf, buf_size, "%lu", value);
        return;
    }

    while (value >= 1024UL && unit + 1U < sizeof(units)) {
        rem = value % 1024UL;
        value /= 1024UL;
        unit++;
    }

    if (unit == 0U || value >= 10UL) {
        snprintf(buf, buf_size, "%lu%c", value, units[unit]);
    } else {
        unsigned long decimal = (rem * 10UL) / 1024UL;
        snprintf(buf, buf_size, "%lu.%lu%c", value, decimal, units[unit]);
    }
}

static char ls_classify_suffix(mode_t mode)
{
    if (S_ISDIR(mode))
        return '/';
    if (S_ISLNK(mode))
        return '@';
    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return '*';
    return '\0';
}

static void ls_print_name(const ls_entry_t *entry, const ls_opts_t *opts)
{
    fputs(entry->name, stdout);
    if (opts->classify && entry->stat_ok) {
        char suffix = ls_classify_suffix(entry->st.st_mode);
        if (suffix != '\0')
            fputc((int)suffix, stdout);
    }
}

static void ls_print_entry(const ls_entry_t *entry, const ls_opts_t *opts)
{
    if (opts->long_format) {
        char mode[11];
        char size_buf[32];

        if (entry->stat_ok) {
            ls_mode_string(entry->st.st_mode, mode);
            ls_format_size(entry->st.st_size, opts->human, size_buf, sizeof(size_buf));
            printf("%s %lu %u %u %s %ld ",
                   mode,
                   (unsigned long)entry->st.st_nlink,
                   (unsigned int)entry->st.st_uid,
                   (unsigned int)entry->st.st_gid,
                   size_buf,
                   (long)entry->st.st_mtime);
        } else {
            printf("?????????? ? ? ? ? ? ");
        }
    }

    ls_print_name(entry, opts);
    fputc('\n', stdout);
}

static void ls_free_entries(ls_entry_t *entries, size_t count)
{
    size_t i;

    if (!entries)
        return;
    for (i = 0U; i < count; i++) {
        free(entries[i].name);
        free(entries[i].full_path);
    }
    free(entries);
}

static int ls_push_entry(ls_entry_t **entries, size_t *count, size_t *cap,
                         const char *name, const char *full_path,
                         const struct stat *st, int stat_ok)
{
    ls_entry_t *grown;

    if (!entries || !count || !cap || !name || !full_path)
        return -1;

    if (*count == *cap) {
        size_t next_cap = (*cap == 0U) ? 16U : (*cap * 2U);
        grown = (ls_entry_t *)realloc(*entries, next_cap * sizeof(**entries));
        if (!grown)
            return -1;
        *entries = grown;
        *cap = next_cap;
    }

    (*entries)[*count].name = ls_strdup_range(name);
    (*entries)[*count].full_path = ls_strdup_range(full_path);
    if (!(*entries)[*count].name || !(*entries)[*count].full_path)
        return -1;

    if (st)
        (*entries)[*count].st = *st;
    else
        memset(&(*entries)[*count].st, 0, sizeof((*entries)[*count].st));
    (*entries)[*count].stat_ok = stat_ok;
    (*count)++;
    return 0;
}

static int ls_should_skip_name(const char *name, const ls_opts_t *opts)
{
    if (!name || !opts)
        return 1;
    if (!opts->show_all && name[0] == '.')
        return 1;
    return 0;
}

static int ls_list_directory(const char *path, const ls_opts_t *opts,
                             int print_header, int depth, int *had_error);

static int ls_handle_path(const char *path, const ls_opts_t *opts,
                          int print_header, int depth, int *had_error)
{
    struct stat st;
    int         rc;

    rc = lstat(path, &st);
    if (rc < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        if (had_error)
            *had_error = 1;
        return -1;
    }

    if (S_ISDIR(st.st_mode) && !opts->directory_as_file)
        return ls_list_directory(path, opts, print_header, depth, had_error);

    if (print_header)
        printf("%s:\n", path);

    if (opts->long_format) {
        ls_entry_t file_entry;

        memset(&file_entry, 0, sizeof(file_entry));
        file_entry.name = (char *)path;
        file_entry.full_path = (char *)path;
        file_entry.st = st;
        file_entry.stat_ok = 1;
        ls_print_entry(&file_entry, opts);
    } else {
        fputs(path, stdout);
        if (opts->classify) {
            char suffix = ls_classify_suffix(st.st_mode);
            if (suffix != '\0')
                fputc((int)suffix, stdout);
        }
        fputc('\n', stdout);
    }

    if (print_header)
        fputc('\n', stdout);
    return 0;
}

static int ls_list_directory(const char *path, const ls_opts_t *opts,
                             int print_header, int depth, int *had_error)
{
    DIR        *dir;
    ls_entry_t *entries = NULL;
    size_t      count = 0U;
    size_t      cap = 0U;
    int         rc = 0;
    size_t      i;

    dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        if (had_error)
            *had_error = 1;
        return -1;
    }

    for (;;) {
        struct dirent *ent = readdir(dir);
        struct stat    st;
        char          *full_path;
        int            stat_ok = 0;

        if (!ent)
            break;
        if (ls_should_skip_name(ent->d_name, opts))
            continue;

        full_path = ls_join_path(path, ent->d_name);
        if (!full_path) {
            if (had_error)
                *had_error = 1;
            rc = -1;
            break;
        }

        if (lstat(full_path, &st) == 0)
            stat_ok = 1;
        else
            memset(&st, 0, sizeof(st));

        if (ls_push_entry(&entries, &count, &cap, ent->d_name, full_path, &st, stat_ok) < 0) {
            free(full_path);
            if (had_error)
                *had_error = 1;
            rc = -1;
            break;
        }

        free(full_path);
    }

    (void)closedir(dir);

    if (rc < 0) {
        ls_free_entries(entries, count);
        return -1;
    }

    qsort(entries, count, sizeof(*entries), ls_cmp_entry);

    if (print_header || depth > 0)
        printf("%s:\n", path);

    for (i = 0U; i < count; i++)
        ls_print_entry(&entries[i], opts);

    if (opts->recursive) {
        for (i = 0U; i < count; i++) {
            if (!entries[i].stat_ok || !S_ISDIR(entries[i].st.st_mode))
                continue;
            if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0)
                continue;
            fputc('\n', stdout);
            (void)ls_list_directory(entries[i].full_path, opts, 1, depth + 1, had_error);
        }
    }

    ls_free_entries(entries, count);
    return 0;
}

int main(int argc, char **argv)
{
    ls_opts_t   opts;
    const char *paths[64];
    size_t      path_count = 0U;
    int         had_error = 0;
    int         i;

    memset(&opts, 0, sizeof(opts));

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!arg)
            continue;
        if (strcmp(arg, "--help") == 0) {
            ls_usage(stdout);
            return 0;
        }
        if (strcmp(arg, "--version") == 0) {
            ls_version();
            return 0;
        }
        if (strcmp(arg, "--") == 0) {
            i++;
            for (; i < argc && path_count < (sizeof(paths) / sizeof(paths[0])); i++)
                paths[path_count++] = argv[i];
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            const char *p = arg + 1;

            while (*p != '\0') {
                switch (*p) {
                case 'a': opts.show_all = 1; break;
                case 'l': opts.long_format = 1; break;
                case 'h': opts.human = 1; break;
                case 'F': opts.classify = 1; break;
                case 'R': opts.recursive = 1; break;
                case 'd': opts.directory_as_file = 1; break;
                case '1': break;
                default:
                    fprintf(stderr, "ls: opzione non supportata -- %c\n", *p);
                    fprintf(stderr, "Prova 'ls --help'.\n");
                    return 1;
                }
                p++;
            }
            continue;
        }
        if (path_count >= (sizeof(paths) / sizeof(paths[0]))) {
            fprintf(stderr, "ls: troppi path\n");
            return 1;
        }
        paths[path_count++] = arg;
    }

    if (path_count == 0U)
        paths[path_count++] = ".";

    for (i = 0; i < (int)path_count; i++) {
        int print_header = (path_count > 1U) ? 1 : 0;

        if (ls_handle_path(paths[i], &opts, print_header, 0, &had_error) < 0 && i + 1 < (int)path_count)
            fputc('\n', stdout);
        else if (print_header && i + 1 < (int)path_count)
            fputc('\n', stdout);
    }

    return had_error ? 1 : 0;
}
