/*
 * EnlilOS Microkernel - Virtual File System Bootstrap (M5-02)
 *
 * Mount table statica:
 *   /        -> initrd CPIO embedded (ro)
 *   /dev     -> devfs
 *   /data    -> ext4 rw-full (se mount riuscito)
 *   /sysroot -> ext4 rw-full (se mount riuscito)
 */

#include "vfs.h"
#include "blk.h"
#include "ext4.h"
#include "initrd.h"
#include "procfs.h"
#include "term80.h"
#include "tty.h"
#include "uart.h"

extern void *memset(void *dst, int value, size_t n);

#define DEV_NODE_DIR        1U
#define DEV_NODE_CONSOLE    2U
#define DEV_NODE_TTY        3U
#define DEV_NODE_STDIN      4U
#define DEV_NODE_STDOUT     5U
#define DEV_NODE_STDERR     6U
#define DEV_NODE_NULL       7U
#define DEV_NODE_ZERO       8U
#define DEV_NODE_URANDOM    9U
#define DEV_NODE_RANDOM     10U

#define PLACE_NODE_DIR      1U
#define PLACE_NODE_README   2U
#define BINDFS_NODE_PROXY   1U

#define BINDFS_HANDLE_MAX   32U
#define VFS_PATH_MAX        256U
#define VFS_SYMLINK_MAX     16U

static vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];
static size_t      vfs_mount_count;
static bool        vfs_initialized;

typedef struct {
    uint8_t    in_use;
    uint8_t    _pad0[3];
    vfs_file_t file;
} bindfs_handle_t;

static bindfs_handle_t bindfs_handles[BINDFS_HANDLE_MAX];
static uint64_t        devfs_rand_state = 0xA5A55A5AF00DFACEULL;

static int path_matches_mount(const char *path, const char *mount_path);
static const char *relpath_from_mount(const vfs_mount_t *mount, const char *path);
static int vfs_open_raw(const char *path, uint32_t flags, vfs_file_t *out);
static int vfs_readlink_raw(const char *path, char *out, size_t cap);
static int vfs_lstat_raw(const char *path, stat_t *out);
static int vfs_utimens_raw(const char *path, const timespec_t *times, uint32_t flags);

static size_t vfs_strlen(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int vfs_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == *b);
}

static void vfs_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static uint64_t devfs_random_next(void)
{
    devfs_rand_state ^= devfs_rand_state << 7;
    devfs_rand_state ^= devfs_rand_state >> 9;
    devfs_rand_state ^= devfs_rand_state << 8;
    return devfs_rand_state;
}

static void vfs_memset(void *dst, uint8_t value, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = value;
}

static void vfs_strlcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    if (max == 0) return;
    while (src && src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void stat_fill(stat_t *st, uint32_t mode, uint64_t size,
                      uint32_t blksize)
{
    if (!st) return;
    st->st_mode    = mode;
    st->st_blksize = blksize;
    st->st_size    = size;
    st->st_blocks  = (size + (uint64_t)blksize - 1ULL) / (uint64_t)blksize;
}

static int wants_write(uint32_t flags)
{
    uint32_t access = (flags & 0x3U);
    return (access == O_WRONLY || access == O_RDWR ||
            (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0U);
}

static void file_reset(vfs_file_t *file, const vfs_mount_t *mount,
                       uint32_t node_id, uint32_t flags)
{
    file->mount     = mount;
    file->node_id   = node_id;
    file->flags     = flags;
    file->pos       = 0;
    file->size_hint = 0;
    file->dir_index = 0;
    file->cookie    = 0;
}

static void file_bind_mem(vfs_file_t *file, const vfs_mount_t *mount,
                          uint32_t node_id, uint32_t flags,
                          const char *content)
{
    file_reset(file, mount, node_id, flags);
    file->cookie    = (uintptr_t)content;
    file->size_hint = vfs_strlen(content);
}

static ssize_t memfile_read(vfs_file_t *file, void *buf, size_t count)
{
    const char *src;
    size_t      remain;

    if (!buf) return -EFAULT;
    if (file->pos >= file->size_hint) return 0;

    remain = (size_t)(file->size_hint - file->pos);
    if (count > remain) count = remain;

    src = (const char *)(uintptr_t)file->cookie + file->pos;
    vfs_memcpy(buf, src, count);
    file->pos += count;
    return (ssize_t)count;
}

static int file_stat_mem(vfs_file_t *file, stat_t *out)
{
    stat_fill(out, S_IFREG | S_IRUSR | S_IRGRP | S_IROTH,
              file->size_hint, 512U);
    return 0;
}

static int file_stat_dir(stat_t *out)
{
    stat_fill(out, S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH,
              0, 512U);
    return 0;
}

static int dirent_fill(vfs_dirent_t *out, const char *name, uint32_t mode)
{
    if (!out) return -EFAULT;
    vfs_strlcpy(out->name, name, sizeof(out->name));
    out->mode = mode;
    return 0;
}

static int vfs_path_join(const char *base, const char *rel,
                         char *out, size_t cap)
{
    size_t base_len = vfs_strlen(base);
    size_t rel_off = 0U;
    size_t pos = 0U;

    if (!base || !out || cap < 2U || base_len == 0U)
        return -EINVAL;

    while (rel && rel[rel_off] == '/')
        rel_off++;

    if (base[0] == '/' && base[1] == '\0') {
        out[0] = '/';
        pos = 1U;
    } else {
        if (base_len + 1U > cap)
            return -ENAMETOOLONG;
        vfs_strlcpy(out, base, cap);
        pos = vfs_strlen(out);
    }

    if (!rel || rel[rel_off] == '\0') {
        if (pos == 0U) {
            out[0] = '/';
            out[1] = '\0';
        } else {
            out[pos] = '\0';
        }
        return 0;
    }

    if (!(pos == 1U && out[0] == '/')) {
        if (pos + 1U >= cap)
            return -ENAMETOOLONG;
        if (out[pos - 1U] != '/')
            out[pos++] = '/';
    }

    while (rel[rel_off] != '\0' && pos + 1U < cap)
        out[pos++] = rel[rel_off++];
    if (rel[rel_off] != '\0')
        return -ENAMETOOLONG;

    out[pos] = '\0';
    return 0;
}

static void vfs_path_pop_last(char *path)
{
    size_t len;

    if (!path || path[0] != '/')
        return;

    len = vfs_strlen(path);
    while (len > 1U && path[len - 1U] == '/')
        len--;
    while (len > 1U && path[len - 1U] != '/')
        len--;

    if (len <= 1U) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    path[len - 1U] = '\0';
}

static int vfs_path_append_component(char *path, const char *comp, size_t comp_len,
                                     size_t cap)
{
    size_t len;
    size_t pos;

    if (!path || !comp || comp_len == 0U || cap < 2U)
        return -EINVAL;

    len = vfs_strlen(path);
    if (len == 0U || path[0] != '/')
        return -EINVAL;

    pos = len;
    if (!(len == 1U && path[0] == '/')) {
        if (pos + 1U >= cap)
            return -ENAMETOOLONG;
        path[pos++] = '/';
    }

    if (pos + comp_len + 1U > cap)
        return -ENAMETOOLONG;

    for (size_t i = 0U; i < comp_len; i++)
        path[pos++] = comp[i];
    path[pos] = '\0';
    return 0;
}

static int vfs_path_dirname(const char *path, char *out, size_t cap)
{
    if (!path || !out || cap < 2U)
        return -EINVAL;
    if (vfs_strlen(path) + 1U > cap)
        return -ENAMETOOLONG;

    vfs_strlcpy(out, path, cap);
    vfs_path_pop_last(out);
    return 0;
}

static int vfs_has_mount_descendant(const char *path)
{
    size_t len;

    if (!path || path[0] != '/')
        return 0;

    len = vfs_strlen(path);
    for (size_t i = 0U; i < vfs_mount_count; i++) {
        const vfs_mount_t *mount = &vfs_mounts[i];

        if (!mount->active || !mount->path)
            continue;
        if (path[0] == '/' && path[1] == '\0') {
            if (!(mount->path[0] == '/' && mount->path[1] == '\0'))
                return 1;
            continue;
        }
        for (size_t j = 0U; j < len; j++) {
            if (mount->path[j] != path[j])
                goto next_mount;
        }
        if (mount->path[len] == '/')
            return 1;
next_mount:
        ;
    }

    return 0;
}

static int vfs_join_symlink_target(const char *link_path, const char *target,
                                   const char *remaining, char *out, size_t cap)
{
    char base[VFS_PATH_MAX];
    int  rc;

    if (!link_path || !target || !out || cap < 2U)
        return -EINVAL;

    if (target[0] == '/') {
        if (vfs_strlen(target) + 1U > sizeof(base))
            return -ENAMETOOLONG;
        vfs_strlcpy(base, target, sizeof(base));
    } else {
        rc = vfs_path_dirname(link_path, base, sizeof(base));
        if (rc < 0)
            return rc;
        rc = vfs_path_join(base, target, base, sizeof(base));
        if (rc < 0)
            return rc;
    }

    if (!remaining || remaining[0] == '\0') {
        if (vfs_strlen(base) + 1U > cap)
            return -ENAMETOOLONG;
        vfs_strlcpy(out, base, cap);
        return 0;
    }

    return vfs_path_join(base, remaining, out, cap);
}

static int vfs_resolve_path_flags(const char *path, bool follow_final,
                                  bool allow_missing_final,
                                  char *out, size_t cap)
{
    char pending[VFS_PATH_MAX];
    char resolved[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];
    char merged[VFS_PATH_MAX];
    uint32_t links = 0U;

    if (!path || !out || cap < 2U)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;
    if (vfs_strlen(path) + 1U > sizeof(pending))
        return -ENAMETOOLONG;

    vfs_strlcpy(pending, path, sizeof(pending));

restart:
    resolved[0] = '/';
    resolved[1] = '\0';

    size_t pos = 0U;
    while (pending[pos] == '/')
        pos++;

    if (pending[pos] == '\0') {
        vfs_strlcpy(out, "/", cap);
        return 0;
    }

    while (pending[pos] != '\0') {
        stat_t st;
        size_t comp_start;
        size_t comp_len;
        bool   has_more;
        int    rc;

        comp_start = pos;
        while (pending[pos] != '\0' && pending[pos] != '/')
            pos++;
        comp_len = pos - comp_start;
        while (pending[pos] == '/')
            pos++;
        has_more = (pending[pos] != '\0');

        if (comp_len == 0U)
            continue;
        if (comp_len == 1U && pending[comp_start] == '.')
            continue;
        if (comp_len == 2U &&
            pending[comp_start] == '.' &&
            pending[comp_start + 1U] == '.') {
            vfs_path_pop_last(resolved);
            continue;
        }

        rc = vfs_path_append_component(resolved, pending + comp_start, comp_len,
                                       sizeof(resolved));
        if (rc < 0)
            return rc;

        rc = vfs_lstat_raw(resolved, &st);
        if (rc == -ENOENT) {
            if (has_more && vfs_has_mount_descendant(resolved))
                continue;
            if (!has_more && allow_missing_final) {
                if (vfs_strlen(resolved) + 1U > cap)
                    return -ENAMETOOLONG;
                vfs_strlcpy(out, resolved, cap);
                return 0;
            }
            return rc;
        }
        if (rc < 0)
            return rc;

        if ((st.st_mode & S_IFMT) == S_IFLNK && (has_more || follow_final)) {
            rc = vfs_readlink_raw(resolved, target, sizeof(target));
            if (rc < 0)
                return rc;
            if (++links > VFS_SYMLINK_MAX)
                return -ELOOP;
            rc = vfs_join_symlink_target(resolved, target,
                                         has_more ? (pending + pos) : "",
                                         merged, sizeof(merged));
            if (rc < 0)
                return rc;
            vfs_strlcpy(pending, merged, sizeof(pending));
            goto restart;
        }

        if (has_more && (st.st_mode & S_IFMT) != S_IFDIR)
            return -ENOTDIR;
    }

    if (vfs_strlen(resolved) + 1U > cap)
        return -ENAMETOOLONG;
    vfs_strlcpy(out, resolved, cap);
    return 0;
}

static bindfs_handle_t *bindfs_handle_get(uintptr_t cookie)
{
    uint32_t idx;

    if (cookie == 0U)
        return NULL;
    idx = (uint32_t)cookie - 1U;
    if (idx >= BINDFS_HANDLE_MAX || !bindfs_handles[idx].in_use)
        return NULL;
    return &bindfs_handles[idx];
}

static bindfs_handle_t *bindfs_handle_alloc(uintptr_t *cookie_out)
{
    for (uint32_t i = 0U; i < BINDFS_HANDLE_MAX; i++) {
        if (bindfs_handles[i].in_use)
            continue;

        memset(&bindfs_handles[i], 0, sizeof(bindfs_handles[i]));
        bindfs_handles[i].in_use = 1U;
        if (cookie_out)
            *cookie_out = (uintptr_t)(i + 1U);
        return &bindfs_handles[i];
    }
    return NULL;
}

static void bindfs_handle_free(bindfs_handle_t *handle)
{
    if (!handle)
        return;
    if (handle->in_use && handle->file.mount)
        (void)vfs_close(&handle->file);
    memset(handle, 0, sizeof(*handle));
}

static int bindfs_resolve_backend(const vfs_mount_t *mount, const char *relpath,
                                  char *backend, size_t cap)
{
    const char *base = (const char *)(uintptr_t)mount->ctx;
    const char *rel = relpath;

    if (!base || !backend || cap < 2U)
        return -EINVAL;
    if (!rel || rel[0] == '\0' || (rel[0] == '/' && rel[1] == '\0')) {
        vfs_strlcpy(backend, base, cap);
        return 0;
    }

    return vfs_path_join(base, rel, backend, cap);
}

static const vfs_mount_t *find_mount_excluding(const char *path,
                                               const vfs_mount_t *exclude)
{
    const vfs_mount_t *best = NULL;
    size_t             best_len = 0U;

    for (size_t i = 0; i < vfs_mount_count; i++) {
        const vfs_mount_t *mount = &vfs_mounts[i];
        size_t             len;

        if (!mount->active || mount == exclude)
            continue;
        if (!path_matches_mount(path, mount->path))
            continue;

        len = vfs_strlen(mount->path);
        if (len >= best_len) {
            best = mount;
            best_len = len;
        }
    }

    return best;
}

static int vfs_open_shadowed(const char *path, const vfs_mount_t *exclude,
                             uint32_t flags, vfs_file_t *out)
{
    const vfs_mount_t *mount;

    if (!path || !out)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;

    mount = find_mount_excluding(path, exclude);
    if (!mount || !mount->ops || !mount->ops->open)
        return -ENOENT;

    return mount->ops->open(mount, relpath_from_mount(mount, path), flags, out);
}

static int vfs_lstat_shadowed(const char *path, const vfs_mount_t *exclude,
                              stat_t *out)
{
    const vfs_mount_t *mount;
    vfs_file_t         file;
    int                rc;

    if (!path || !out)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;

    mount = find_mount_excluding(path, exclude);
    if (!mount || !mount->ops)
        return -ENOENT;
    if (mount->ops->lstat)
        return mount->ops->lstat(mount, relpath_from_mount(mount, path), out);
    if (!mount->ops->open)
        return -ENOSYS;

    rc = mount->ops->open(mount, relpath_from_mount(mount, path), O_RDONLY, &file);
    if (rc < 0)
        return rc;
    rc = mount->ops->stat ? mount->ops->stat(&file, out) : -ENOSYS;
    if (mount->ops->close)
        (void)mount->ops->close(&file);
    return rc;
}

static int vfs_readlink_shadowed(const char *path, const vfs_mount_t *exclude,
                                 char *out, size_t cap)
{
    const vfs_mount_t *mount;

    if (!path || !out || cap < 2U)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;

    mount = find_mount_excluding(path, exclude);
    if (!mount || !mount->ops || !mount->ops->readlink)
        return -ENOENT;

    return mount->ops->readlink(mount, relpath_from_mount(mount, path), out, cap);
}

static int vfs_utimens_shadowed(const char *path, const vfs_mount_t *exclude,
                                const timespec_t *times, uint32_t flags)
{
    const vfs_mount_t *mount;

    if (!path)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;

    mount = find_mount_excluding(path, exclude);
    if (!mount)
        return -ENOENT;
    if (mount->readonly)
        return -EROFS;
    if (!mount->ops || !mount->ops->utimens)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->utimens(mount, relpath_from_mount(mount, path), times, flags);
}

/* ── devfs ───────────────────────────────────────────────────────── */

static int devfs_open(const vfs_mount_t *mount, const char *relpath,
                      uint32_t flags, vfs_file_t *out)
{
    const char *name = relpath;

    if (!out) return -EFAULT;
    if (name[0] == '/') name++;

    if (name[0] == '\0') {
        file_reset(out, mount, DEV_NODE_DIR, flags);
        return 0;
    }
    if (vfs_streq(name, "console")) {
        file_reset(out, mount, DEV_NODE_CONSOLE, flags);
        return 0;
    }
    if (vfs_streq(name, "tty")) {
        file_reset(out, mount, DEV_NODE_TTY, flags);
        return 0;
    }
    if (vfs_streq(name, "stdin")) {
        file_reset(out, mount, DEV_NODE_STDIN, flags);
        return 0;
    }
    if (vfs_streq(name, "stdout")) {
        file_reset(out, mount, DEV_NODE_STDOUT, flags);
        return 0;
    }
    if (vfs_streq(name, "stderr")) {
        file_reset(out, mount, DEV_NODE_STDERR, flags);
        return 0;
    }
    if (vfs_streq(name, "null")) {
        file_reset(out, mount, DEV_NODE_NULL, flags);
        return 0;
    }
    if (vfs_streq(name, "zero")) {
        file_reset(out, mount, DEV_NODE_ZERO, flags);
        return 0;
    }
    if (vfs_streq(name, "urandom")) {
        file_reset(out, mount, DEV_NODE_URANDOM, flags);
        return 0;
    }
    if (vfs_streq(name, "random")) {
        file_reset(out, mount, DEV_NODE_RANDOM, flags);
        return 0;
    }

    return -ENOENT;
}

static ssize_t devfs_read(vfs_file_t *file, void *buf, size_t count)
{
    if (file->node_id == DEV_NODE_DIR)
        return -EISDIR;
    if (!buf) return -EFAULT;
    if (file->node_id == DEV_NODE_NULL)
        return 0;
    if (file->node_id == DEV_NODE_ZERO) {
        vfs_memset(buf, 0U, count);
        return (ssize_t)count;
    }
    if (file->node_id == DEV_NODE_URANDOM || file->node_id == DEV_NODE_RANDOM) {
        uint8_t *dst = (uint8_t *)buf;
        size_t   pos = 0U;

        while (pos < count) {
            uint64_t word = devfs_random_next();
            for (uint32_t i = 0U; i < 8U && pos < count; i++, pos++)
                dst[pos] = (uint8_t)(word >> (i * 8U));
        }
        return (ssize_t)count;
    }
    if (count > 4096U) count = 4096U;
    return (ssize_t)tty_read((char *)buf, count);
}

static ssize_t devfs_write(vfs_file_t *file, const void *buf, size_t count)
{
    const char *bytes = (const char *)buf;

    if (file->node_id == DEV_NODE_DIR)
        return -EISDIR;
    if (!buf) return -EFAULT;
    if (file->node_id == DEV_NODE_NULL)
        return (ssize_t)count;
    if (tty_check_output_current() < 0)
        return -EINTR;

    if (count > 4096U) count = 4096U;
    term80_write(bytes, (uint32_t)count);
    for (size_t i = 0; i < count; i++)
        uart_putc(bytes[i]);

    return (ssize_t)count;
}

static int devfs_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    static const char *const names[] = {
        "console", "tty", "stdin", "stdout", "stderr", "null",
        "zero", "urandom", "random"
    };

    if (file->node_id != DEV_NODE_DIR)
        return -ENOTDIR;
    if (file->dir_index >= (uint32_t)(sizeof(names) / sizeof(names[0])))
        return -ENOENT;

    return dirent_fill(out, names[file->dir_index++],
                       S_IFCHR | S_IRUSR | S_IWUSR |
                       S_IRGRP | S_IWGRP |
                       S_IROTH | S_IWOTH);
}

static int devfs_stat(vfs_file_t *file, stat_t *out)
{
    if (file->node_id == DEV_NODE_DIR)
        return file_stat_dir(out);

    stat_fill(out, S_IFCHR | S_IRUSR | S_IWUSR |
                    S_IRGRP | S_IWGRP |
                    S_IROTH | S_IWOTH, 0, 1U);
    return 0;
}

static int devfs_close(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static const vfs_ops_t devfs_ops = {
    .open    = devfs_open,
    .read    = devfs_read,
    .write   = devfs_write,
    .readdir = devfs_readdir,
    .stat    = devfs_stat,
    .close   = devfs_close,
};

/* ── info mount fallback (ext4 assente / errore mount) ───────────── */

static int placefs_open(const vfs_mount_t *mount, const char *relpath,
                        uint32_t flags, vfs_file_t *out)
{
    const char *name = relpath;

    if (!out) return -EFAULT;
    if (name[0] == '/') name++;

    if (name[0] == '\0') {
        file_reset(out, mount, PLACE_NODE_DIR, flags);
        return 0;
    }
    if (wants_write(flags))
        return -EROFS;

    if (vfs_streq(name, "README.TXT")) {
        file_bind_mem(out, mount, PLACE_NODE_README, flags,
                      (const char *)(uintptr_t)mount->ctx);
        return 0;
    }

    return -ENOENT;
}

static ssize_t placefs_read(vfs_file_t *file, void *buf, size_t count)
{
    if (file->node_id == PLACE_NODE_DIR)
        return -EISDIR;
    return memfile_read(file, buf, count);
}

static ssize_t placefs_write(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    return -EROFS;
}

static int placefs_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    if (file->node_id != PLACE_NODE_DIR)
        return -ENOTDIR;
    if (file->dir_index++ > 0U)
        return -ENOENT;

    return dirent_fill(out, "README.TXT",
                       S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
}

static int placefs_stat(vfs_file_t *file, stat_t *out)
{
    if (file->node_id == PLACE_NODE_DIR)
        return file_stat_dir(out);
    return file_stat_mem(file, out);
}

static int placefs_close(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static const vfs_ops_t placefs_ops = {
    .open    = placefs_open,
    .read    = placefs_read,
    .write   = placefs_write,
    .readdir = placefs_readdir,
    .stat    = placefs_stat,
    .close   = placefs_close,
};

/* ── bindfs (mount trasparenti verso un backend path) ───────────── */

static int bindfs_open(const vfs_mount_t *mount, const char *relpath,
                       uint32_t flags, vfs_file_t *out)
{
    bindfs_handle_t *handle;
    uintptr_t        cookie = 0U;
    char             backend[128];
    char             fallback[128];
    int              rc;

    if (!out)
        return -EFAULT;
    rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));
    if (rc < 0)
        return rc;

    handle = bindfs_handle_alloc(&cookie);
    if (!handle)
        return -ENFILE;

    rc = vfs_open(backend, flags, &handle->file);
    if ((rc == -ENOENT || rc == -ENOTDIR) &&
        vfs_path_join(mount->path, relpath, fallback, sizeof(fallback)) == 0) {
        rc = vfs_open_shadowed(fallback, mount, flags, &handle->file);
    }
    if (rc < 0) {
        bindfs_handle_free(handle);
        return rc;
    }

    file_reset(out, mount, BINDFS_NODE_PROXY, flags);
    out->cookie = cookie;
    return 0;
}

static ssize_t bindfs_read(vfs_file_t *file, void *buf, size_t count)
{
    bindfs_handle_t *handle = bindfs_handle_get(file ? file->cookie : 0U);

    if (!handle)
        return -EBADF;
    return vfs_read(&handle->file, buf, count);
}

static ssize_t bindfs_write(vfs_file_t *file, const void *buf, size_t count)
{
    bindfs_handle_t *handle = bindfs_handle_get(file ? file->cookie : 0U);

    if (!handle)
        return -EBADF;
    return vfs_write(&handle->file, buf, count);
}

static int bindfs_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    bindfs_handle_t *handle = bindfs_handle_get(file ? file->cookie : 0U);

    if (!handle)
        return -EBADF;
    return vfs_readdir(&handle->file, out);
}

static int bindfs_stat(vfs_file_t *file, stat_t *out)
{
    bindfs_handle_t *handle = bindfs_handle_get(file ? file->cookie : 0U);

    if (!handle)
        return -EBADF;
    return vfs_stat(&handle->file, out);
}

static int bindfs_close(vfs_file_t *file)
{
    bindfs_handle_t *handle = bindfs_handle_get(file ? file->cookie : 0U);

    if (!handle)
        return -EBADF;
    bindfs_handle_free(handle);
    file->cookie = 0U;
    return 0;
}

static int bindfs_mkdir(const vfs_mount_t *mount, const char *relpath,
                        uint32_t mode)
{
    char backend[128];
    int  rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));

    if (rc < 0)
        return rc;
    return vfs_mkdir(backend, mode);
}

static int bindfs_unlink(const vfs_mount_t *mount, const char *relpath)
{
    char backend[128];
    int  rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));

    if (rc < 0)
        return rc;
    return vfs_unlink(backend);
}

static int bindfs_symlink(const vfs_mount_t *mount, const char *target,
                          const char *relpath)
{
    char backend[128];
    int  rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));

    if (rc < 0)
        return rc;
    return vfs_symlink(target, backend);
}

static int bindfs_readlink(const vfs_mount_t *mount, const char *relpath,
                           char *out, size_t cap)
{
    char backend[128];
    char fallback[128];
    int  rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));

    if (rc < 0)
        return rc;
    rc = vfs_readlink(backend, out, cap);
    if ((rc == -ENOENT || rc == -ENOTDIR) &&
        vfs_path_join(mount->path, relpath, fallback, sizeof(fallback)) == 0) {
        rc = vfs_readlink_shadowed(fallback, mount, out, cap);
    }
    return rc;
}

static int bindfs_lstat(const vfs_mount_t *mount, const char *relpath,
                        stat_t *out)
{
    char backend[128];
    char fallback[128];
    int  rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));

    if (rc < 0)
        return rc;
    rc = vfs_lstat(backend, out);
    if ((rc == -ENOENT || rc == -ENOTDIR) &&
        vfs_path_join(mount->path, relpath, fallback, sizeof(fallback)) == 0) {
        rc = vfs_lstat_shadowed(fallback, mount, out);
    }
    return rc;
}

static int bindfs_rename(const vfs_mount_t *old_mount, const char *old_relpath,
                         const vfs_mount_t *new_mount, const char *new_relpath)
{
    char old_backend[128];
    char new_backend[128];
    int  rc;

    rc = bindfs_resolve_backend(old_mount, old_relpath,
                                old_backend, sizeof(old_backend));
    if (rc < 0)
        return rc;
    rc = bindfs_resolve_backend(new_mount, new_relpath,
                                new_backend, sizeof(new_backend));
    if (rc < 0)
        return rc;
    return vfs_rename(old_backend, new_backend);
}

static int bindfs_fsync(vfs_file_t *file)
{
    bindfs_handle_t *handle = bindfs_handle_get(file ? file->cookie : 0U);

    if (!handle)
        return -EBADF;
    return vfs_fsync(&handle->file);
}

static int bindfs_truncate(const vfs_mount_t *mount, const char *relpath,
                           uint64_t size)
{
    char backend[128];
    int  rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));

    if (rc < 0)
        return rc;
    return vfs_truncate(backend, size);
}

static int bindfs_utimens(const vfs_mount_t *mount, const char *relpath,
                          const timespec_t *times, uint32_t flags)
{
    char backend[128];
    char fallback[128];
    int  rc = bindfs_resolve_backend(mount, relpath, backend, sizeof(backend));

    if (rc < 0)
        return rc;
    rc = vfs_utimens(backend, times, flags);
    if ((rc == -ENOENT || rc == -ENOTDIR) &&
        vfs_path_join(mount->path, relpath, fallback, sizeof(fallback)) == 0) {
        rc = vfs_utimens_shadowed(fallback, mount, times, flags);
    }
    return rc;
}

static int bindfs_sync(const vfs_mount_t *mount)
{
    (void)mount;
    return 0;
}

static const vfs_ops_t bindfs_ops = {
    .open     = bindfs_open,
    .read     = bindfs_read,
    .write    = bindfs_write,
    .readdir  = bindfs_readdir,
    .stat     = bindfs_stat,
    .close    = bindfs_close,
    .mkdir    = bindfs_mkdir,
    .unlink   = bindfs_unlink,
    .symlink  = bindfs_symlink,
    .readlink = bindfs_readlink,
    .lstat    = bindfs_lstat,
    .rename   = bindfs_rename,
    .fsync    = bindfs_fsync,
    .truncate = bindfs_truncate,
    .utimens  = bindfs_utimens,
    .sync     = bindfs_sync,
};

/* ── mount table e path resolution ───────────────────────────────── */

static void mount_register(const char *path, const char *name, const char *fs,
                           bool readonly, bool linux_compat,
                           const vfs_ops_t *ops, uintptr_t ctx)
{
    vfs_mount_t *mount;

    if (vfs_mount_count >= VFS_MAX_MOUNTS)
        return;

    mount = &vfs_mounts[vfs_mount_count++];
    mount->active   = true;
    mount->readonly = readonly;
    mount->linux_compat = linux_compat;
    mount->path     = path;
    mount->name     = name;
    mount->fs_type  = fs;
    mount->ops      = ops;
    mount->ctx      = ctx;
}

static int path_matches_mount(const char *path, const char *mount_path)
{
    size_t i = 0;

    if (mount_path[0] == '/' && mount_path[1] == '\0')
        return path[0] == '/';

    while (mount_path[i]) {
        if (path[i] != mount_path[i])
            return 0;
        i++;
    }

    return (path[i] == '\0' || path[i] == '/');
}

static const vfs_mount_t *find_mount(const char *path)
{
    const vfs_mount_t *best = NULL;
    size_t             best_len = 0;

    for (size_t i = 0; i < vfs_mount_count; i++) {
        const vfs_mount_t *mount = &vfs_mounts[i];
        size_t             len;

        if (!mount->active) continue;
        if (!path_matches_mount(path, mount->path)) continue;

        len = vfs_strlen(mount->path);
        if (len >= best_len) {
            best = mount;
            best_len = len;
        }
    }

    return best;
}

static const char *relpath_from_mount(const vfs_mount_t *mount, const char *path)
{
    size_t len = vfs_strlen(mount->path);

    if (mount->path[0] == '/' && mount->path[1] == '\0')
        return path;

    if (path[len] == '\0')
        return "/";
    return path + len;
}

static void log_mounts(void)
{
    for (size_t i = 0; i < vfs_mount_count; i++) {
        uart_puts("[VFS] mount ");
        uart_puts(vfs_mounts[i].path);
        uart_puts(" -> ");
        uart_puts(vfs_mounts[i].fs_type);
        if (vfs_mounts[i].readonly)
            uart_puts(" (ro)");
        if (vfs_mounts[i].linux_compat)
            uart_puts(" [linux]");
        uart_puts("\n");
    }
}

static void build_mount_table(void)
{
    int ext4_rc = -1;
    int initrd_rc;

    if (blk_is_ready())
        ext4_rc = ext4_mount();
    else
        ext4_unmount();

    initrd_rc = initrd_init();

    vfs_mount_count = 0;
    if (initrd_rc == 0 && initrd_is_ready()) {
        mount_register("/", "bootstrap-root", "initrd-cpio",
                       true, false, initrd_vfs_ops(), 0U);
    } else {
        mount_register("/", "bootstrap-root", "initrd-error",
                       true, false, &placefs_ops, (uintptr_t)initrd_status());
    }
    mount_register("/dev", "device-nodes", "devfs",
                   false, false, &devfs_ops, 0U);
    mount_register("/lib", "linux-lib", "bindfs",
                   true, true, &bindfs_ops, (uintptr_t)"/sysroot/lib");
    mount_register("/usr", "linux-usr", "bindfs",
                   true, true, &bindfs_ops, (uintptr_t)"/sysroot/usr");
    mount_register("/bin/sh", "linux-shell", "bindfs",
                   true, true, &bindfs_ops, (uintptr_t)"/sysroot/usr/bin/bash");

    if (blk_is_ready()) {
        if (ext4_rc == 0 && ext4_is_mounted()) {
            mount_register("/data", "data-root", "ext4",
                           false, false, ext4_vfs_ops(), 0U);
            mount_register("/sysroot", "sysroot-root", "ext4",
                           false, false, ext4_vfs_ops(), 0U);
            mount_register("/var", "linux-var", "bindfs",
                           false, false, &bindfs_ops, (uintptr_t)"/data/var");
            mount_register("/tmp", "linux-tmp", "bindfs",
                           false, false, &bindfs_ops, (uintptr_t)"/data/tmp");
        } else {
            mount_register("/data", "data-root", "ext4-error",
                           true, false, &placefs_ops, (uintptr_t)ext4_status());
            mount_register("/sysroot", "sysroot-root", "ext4-error",
                           true, false, &placefs_ops, (uintptr_t)ext4_status());
        }
    }

    /* === procfs (M14-01) === */
    mount_register("/proc", "process-info", "procfs",
                   true, false, procfs_vfs_ops(), 0U);
}

void vfs_init(void)
{
    build_mount_table();
    vfs_initialized = true;

    uart_puts("[VFS] Bootstrap layer pronta\n");
    log_mounts();
}

void vfs_rescan(void)
{
    if (!vfs_initialized) {
        vfs_init();
        return;
    }

    build_mount_table();
    uart_puts("[VFS] Mount table aggiornata\n");
    log_mounts();
}

static int vfs_open_raw(const char *path, uint32_t flags, vfs_file_t *out)
{
    const vfs_mount_t *mount;

    if (!path || !out) return -EFAULT;
    if (path[0] != '/') return -ENOENT;

    mount = find_mount(path);
    if (!mount || !mount->ops || !mount->ops->open)
        return -ENOENT;

    return mount->ops->open(mount, relpath_from_mount(mount, path), flags, out);
}

int vfs_open(const char *path, uint32_t flags, vfs_file_t *out)
{
    char resolved[VFS_PATH_MAX];
    int  rc;

    rc = vfs_resolve_path_flags(path, true, (flags & O_CREAT) != 0U,
                                resolved, sizeof(resolved));
    if (rc < 0)
        return rc;
    return vfs_open_raw(resolved, flags, out);
}

ssize_t vfs_read(vfs_file_t *file, void *buf, size_t count)
{
    if (!file || !file->mount || !file->mount->ops || !file->mount->ops->read)
        return -EBADF;
    return file->mount->ops->read(file, buf, count);
}

ssize_t vfs_write(vfs_file_t *file, const void *buf, size_t count)
{
    if (!file || !file->mount || !file->mount->ops || !file->mount->ops->write)
        return -EBADF;
    return file->mount->ops->write(file, buf, count);
}

int vfs_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    if (!file || !file->mount || !file->mount->ops || !file->mount->ops->readdir)
        return -EBADF;
    return file->mount->ops->readdir(file, out);
}

int vfs_stat(vfs_file_t *file, stat_t *out)
{
    if (!file || !file->mount || !file->mount->ops || !file->mount->ops->stat)
        return -EBADF;
    return file->mount->ops->stat(file, out);
}

int vfs_close(vfs_file_t *file)
{
    if (!file || !file->mount || !file->mount->ops || !file->mount->ops->close)
        return -EBADF;
    return file->mount->ops->close(file);
}

int vfs_mkdir(const char *path, uint32_t mode)
{
    const vfs_mount_t *mount;
    char               resolved[VFS_PATH_MAX];
    int                rc;

    if (!path) return -EFAULT;
    rc = vfs_resolve_path_flags(path, false, true,
                                resolved, sizeof(resolved));
    if (rc < 0)
        return rc;

    mount = find_mount(resolved);
    if (!mount || !mount->ops || !mount->ops->mkdir)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->mkdir(mount, relpath_from_mount(mount, resolved), mode);
}

int vfs_unlink(const char *path)
{
    const vfs_mount_t *mount;
    char               resolved[VFS_PATH_MAX];
    int                rc;

    if (!path) return -EFAULT;
    rc = vfs_resolve_path_flags(path, false, false,
                                resolved, sizeof(resolved));
    if (rc < 0)
        return rc;

    mount = find_mount(resolved);
    if (!mount || !mount->ops || !mount->ops->unlink)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->unlink(mount, relpath_from_mount(mount, resolved));
}

int vfs_symlink(const char *target, const char *path)
{
    const vfs_mount_t *mount;
    char               resolved[VFS_PATH_MAX];
    int                rc;

    if (!target || !path) return -EFAULT;
    rc = vfs_resolve_path_flags(path, false, true,
                                resolved, sizeof(resolved));
    if (rc < 0)
        return rc;

    mount = find_mount(resolved);
    if (!mount || !mount->ops || !mount->ops->symlink)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->symlink(mount, target, relpath_from_mount(mount, resolved));
}

static int vfs_readlink_raw(const char *path, char *out, size_t cap)
{
    const vfs_mount_t *mount;

    if (!path || !out || cap < 2U) return -EFAULT;
    if (path[0] != '/') return -ENOENT;

    mount = find_mount(path);
    if (!mount || !mount->ops || !mount->ops->readlink)
        return mount && mount->readonly ? -EINVAL : -ENOSYS;

    return mount->ops->readlink(mount, relpath_from_mount(mount, path), out, cap);
}

int vfs_readlink(const char *path, char *out, size_t cap)
{
    char resolved[VFS_PATH_MAX];
    int  rc;

    rc = vfs_resolve_path_flags(path, false, false,
                                resolved, sizeof(resolved));
    if (rc < 0)
        return rc;
    return vfs_readlink_raw(resolved, out, cap);
}

static int vfs_lstat_raw(const char *path, stat_t *out)
{
    const vfs_mount_t *mount;
    vfs_file_t         file;
    int                rc;

    if (!path || !out) return -EFAULT;
    if (path[0] != '/') return -ENOENT;

    mount = find_mount(path);
    if (!mount || !mount->ops)
        return -ENOENT;
    if (mount->ops->lstat)
        return mount->ops->lstat(mount, relpath_from_mount(mount, path), out);

    rc = vfs_open_raw(path, O_RDONLY, &file);
    if (rc < 0)
        return rc;
    rc = vfs_stat(&file, out);
    (void)vfs_close(&file);
    return rc;
}

int vfs_lstat(const char *path, stat_t *out)
{
    char resolved[VFS_PATH_MAX];
    int  rc;

    rc = vfs_resolve_path_flags(path, false, false,
                                resolved, sizeof(resolved));
    if (rc < 0)
        return rc;
    return vfs_lstat_raw(resolved, out);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    const vfs_mount_t *old_mount;
    const vfs_mount_t *new_mount;
    char               old_resolved[VFS_PATH_MAX];
    char               new_resolved[VFS_PATH_MAX];
    int                rc;

    if (!old_path || !new_path) return -EFAULT;
    rc = vfs_resolve_path_flags(old_path, false, false,
                                old_resolved, sizeof(old_resolved));
    if (rc < 0)
        return rc;
    rc = vfs_resolve_path_flags(new_path, false, true,
                                new_resolved, sizeof(new_resolved));
    if (rc < 0)
        return rc;

    old_mount = find_mount(old_resolved);
    new_mount = find_mount(new_resolved);
    if (!old_mount || !new_mount)
        return -ENOENT;
    if (old_mount != new_mount)
        return -EXDEV;
    if (!old_mount->ops || !old_mount->ops->rename)
        return old_mount->readonly ? -EROFS : -ENOSYS;

    return old_mount->ops->rename(old_mount, relpath_from_mount(old_mount, old_resolved),
                                  new_mount, relpath_from_mount(new_mount, new_resolved));
}

int vfs_fsync(vfs_file_t *file)
{
    if (!file || !file->mount || !file->mount->ops || !file->mount->ops->fsync)
        return -EBADF;
    return file->mount->ops->fsync(file);
}

int vfs_truncate(const char *path, uint64_t size)
{
    const vfs_mount_t *mount;
    char               resolved[VFS_PATH_MAX];
    int                rc;

    if (!path) return -EFAULT;
    rc = vfs_resolve_path_flags(path, true, false,
                                resolved, sizeof(resolved));
    if (rc < 0)
        return rc;

    mount = find_mount(resolved);
    if (!mount || !mount->ops || !mount->ops->truncate)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->truncate(mount, relpath_from_mount(mount, resolved), size);
}

static int vfs_utimens_raw(const char *path, const timespec_t *times, uint32_t flags)
{
    const vfs_mount_t *mount;

    if (!path)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;

    mount = find_mount(path);
    if (!mount)
        return -ENOENT;
    if (mount->readonly)
        return -EROFS;
    if (!mount->ops || !mount->ops->utimens)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->utimens(mount, relpath_from_mount(mount, path), times, flags);
}

int vfs_utimens(const char *path, const timespec_t *times, uint32_t flags)
{
    char resolved[VFS_PATH_MAX];
    int  rc;

    if (!path)
        return -EFAULT;
    if (flags & ~VFS_UTIMENS_NOFOLLOW)
        return -EINVAL;

    rc = vfs_resolve_path_flags(path, (flags & VFS_UTIMENS_NOFOLLOW) == 0U,
                                false, resolved, sizeof(resolved));
    if (rc < 0)
        return rc;
    return vfs_utimens_raw(resolved, times, flags);
}

/* Indirizzo minimo valido per un puntatore kernel (testo/dati kernel) */
#define VFS_KERNEL_PTR_MIN  0x40000000UL
#define VFS_KERNEL_PTR_MAX  0xC0000000UL

/* Guard: il puntatore è in range kernel e allineato a 4 byte? */
static int vfs_ptr_sane(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (v >= VFS_KERNEL_PTR_MIN) && (v < VFS_KERNEL_PTR_MAX) && ((v & 3U) == 0U);
}

int vfs_sync(void)
{
    int first_err = 0;

    for (size_t i = 0; i < vfs_mount_count; i++) {
        const vfs_mount_t *mount = &vfs_mounts[i];
        int rc;

        if (!mount->active)
            continue;
        if (!vfs_ptr_sane(mount->ops))
            continue;
        if (!vfs_ptr_sane((const void *)mount->ops->sync))
            continue;

        rc = mount->ops->sync(mount);
        if (rc < 0 && first_err == 0)
            first_err = rc;
    }

    return first_err;
}

int vfs_path_is_linux_compat(const char *path)
{
    const vfs_mount_t *mount;

    if (!path || path[0] != '/')
        return 0;

    mount = find_mount(path);
    return (mount && mount->linux_compat) ? 1 : 0;
}
