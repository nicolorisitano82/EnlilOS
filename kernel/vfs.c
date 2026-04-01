/*
 * EnlilOS Microkernel - Virtual File System Bootstrap (M5-02)
 *
 * Mount table statica:
 *   /        -> initrd-ro bootstrap stub
 *   /dev     -> devfs
 *   /data    -> ext4 read-only (se mount riuscito)
 *   /sysroot -> ext4 read-only (se mount riuscito)
 *
 * M5-03 sostituisce il vecchio placeholder ext4 con un parser read-only
 * sopra virtio-blk, mantenendo la stessa ABI lato syscall/VFS.
 */

#include "vfs.h"
#include "blk.h"
#include "ext4.h"
#include "tty.h"
#include "uart.h"

#define ROOT_NODE_DIR       1U
#define ROOT_NODE_README    2U
#define ROOT_NODE_BOOT      3U
#define ROOT_NODE_DEMO_ELF  4U
#define ROOT_NODE_EXEC1_ELF 5U
#define ROOT_NODE_EXEC2_ELF 6U

#define DEV_NODE_DIR        1U
#define DEV_NODE_CONSOLE    2U
#define DEV_NODE_TTY        3U
#define DEV_NODE_STDIN      4U
#define DEV_NODE_STDOUT     5U
#define DEV_NODE_STDERR     6U
#define DEV_NODE_NULL       7U

#define PLACE_NODE_DIR      1U
#define PLACE_NODE_README   2U

static const char vfs_root_readme[] =
    "EnlilOS bootstrap rootfs.\n"
    "Questo mount `/` e' read-only e prepara il passaggio a un initrd vero.\n"
    "I device node vivono sotto `/dev`.\n";

extern const uint8_t _binary_user_demo_elf_start[];
extern const uint8_t _binary_user_demo_elf_end[];
extern const uint8_t _binary_user_execve_demo_elf_start[];
extern const uint8_t _binary_user_execve_demo_elf_end[];
extern const uint8_t _binary_user_execve_target_elf_start[];
extern const uint8_t _binary_user_execve_target_elf_end[];

static char vfs_root_boot_txt[256];
static vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];
static size_t      vfs_mount_count;
static bool        vfs_initialized;

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

static char *buf_puts(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return dst;
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

static void file_bind_blob(vfs_file_t *file, const vfs_mount_t *mount,
                           uint32_t node_id, uint32_t flags,
                           const void *content, size_t size)
{
    file_reset(file, mount, node_id, flags);
    file->cookie    = (uintptr_t)content;
    file->size_hint = size;
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

static int mount_is_active(const char *path)
{
    for (size_t i = 0; i < vfs_mount_count; i++) {
        if (vfs_mounts[i].active && vfs_streq(vfs_mounts[i].path, path))
            return 1;
    }
    return 0;
}

/* ── initrd bootstrap rootfs ─────────────────────────────────────── */

static int rootfs_open(const vfs_mount_t *mount, const char *relpath,
                       uint32_t flags, vfs_file_t *out)
{
    const char *name = relpath;

    if (!out) return -EFAULT;
    if (name[0] == '/') name++;

    if (name[0] == '\0') {
        file_reset(out, mount, ROOT_NODE_DIR, flags);
        return 0;
    }
    if (wants_write(flags))
        return -EROFS;

    if (vfs_streq(name, "README.TXT")) {
        file_bind_mem(out, mount, ROOT_NODE_README, flags, vfs_root_readme);
        return 0;
    }
    if (vfs_streq(name, "BOOT.TXT")) {
        file_bind_mem(out, mount, ROOT_NODE_BOOT, flags, vfs_root_boot_txt);
        return 0;
    }
    if (vfs_streq(name, "DEMO.ELF")) {
        file_bind_blob(out, mount, ROOT_NODE_DEMO_ELF, flags,
                       _binary_user_demo_elf_start,
                       (size_t)(_binary_user_demo_elf_end - _binary_user_demo_elf_start));
        return 0;
    }
    if (vfs_streq(name, "EXEC1.ELF")) {
        file_bind_blob(out, mount, ROOT_NODE_EXEC1_ELF, flags,
                       _binary_user_execve_demo_elf_start,
                       (size_t)(_binary_user_execve_demo_elf_end -
                                _binary_user_execve_demo_elf_start));
        return 0;
    }
    if (vfs_streq(name, "EXEC2.ELF")) {
        file_bind_blob(out, mount, ROOT_NODE_EXEC2_ELF, flags,
                       _binary_user_execve_target_elf_start,
                       (size_t)(_binary_user_execve_target_elf_end -
                                _binary_user_execve_target_elf_start));
        return 0;
    }

    return -ENOENT;
}

static ssize_t rootfs_read(vfs_file_t *file, void *buf, size_t count)
{
    if (file->node_id == ROOT_NODE_DIR)
        return -EISDIR;
    return memfile_read(file, buf, count);
}

static ssize_t rootfs_write(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    return -EROFS;
}

static int rootfs_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    while (1) {
        uint32_t idx = file->dir_index++;

        switch (idx) {
        case 0:
            return dirent_fill(out, "README.TXT",
                               S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
        case 1:
            return dirent_fill(out, "BOOT.TXT",
                               S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
        case 2:
            return dirent_fill(out, "dev",
                               S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH);
        case 3:
            return dirent_fill(out, "DEMO.ELF",
                               S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
        case 4:
            return dirent_fill(out, "EXEC1.ELF",
                               S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
        case 5:
            return dirent_fill(out, "EXEC2.ELF",
                               S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
        case 6:
            if (mount_is_active("/data"))
                return dirent_fill(out, "data",
                                   S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH);
            break;
        case 7:
            if (mount_is_active("/sysroot"))
                return dirent_fill(out, "sysroot",
                                   S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH);
            break;
        default:
            return -ENOENT;
        }
    }
}

static int rootfs_stat(vfs_file_t *file, stat_t *out)
{
    if (file->node_id == ROOT_NODE_DIR)
        return file_stat_dir(out);
    return file_stat_mem(file, out);
}

static int rootfs_close(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static const vfs_ops_t rootfs_ops = {
    .open    = rootfs_open,
    .read    = rootfs_read,
    .write   = rootfs_write,
    .readdir = rootfs_readdir,
    .stat    = rootfs_stat,
    .close   = rootfs_close,
};

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

    return -ENOENT;
}

static ssize_t devfs_read(vfs_file_t *file, void *buf, size_t count)
{
    if (file->node_id == DEV_NODE_DIR)
        return -EISDIR;
    if (!buf) return -EFAULT;
    if (file->node_id == DEV_NODE_NULL)
        return 0;
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

    if (count > 4096U) count = 4096U;
    for (size_t i = 0; i < count; i++)
        uart_putc(bytes[i]);

    return (ssize_t)count;
}

static int devfs_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    static const char *const names[] = {
        "console", "tty", "stdin", "stdout", "stderr", "null"
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

/* ── mount table e path resolution ───────────────────────────────── */

static void mount_register(const char *path, const char *name, const char *fs,
                           bool readonly, const vfs_ops_t *ops, uintptr_t ctx)
{
    vfs_mount_t *mount;

    if (vfs_mount_count >= VFS_MAX_MOUNTS)
        return;

    mount = &vfs_mounts[vfs_mount_count++];
    mount->active   = true;
    mount->readonly = readonly;
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
        uart_puts("\n");
    }
}

static void build_dynamic_texts(void)
{
    char    *p;

    p = vfs_root_boot_txt;
    p = buf_puts(p, "Mount profile bootstrap:\n");
    p = buf_puts(p, "/        -> initrd-ro stub\n");
    p = buf_puts(p, "/dev     -> devfs\n");
    if (blk_is_ready()) {
        if (ext4_is_mounted()) {
            p = buf_puts(p, "/data    -> ext4 rw-core su virtio-blk\n");
            p = buf_puts(p, "/sysroot -> ext4 rw-core su virtio-blk\n");
        } else {
            p = buf_puts(p, "/data    -> ext4 mount fallito\n");
            p = buf_puts(p, "/sysroot -> ext4 mount fallito\n");
        }
    } else {
        p = buf_puts(p, "/data    -> offline (virtio-blk assente)\n");
        p = buf_puts(p, "/sysroot -> offline (virtio-blk assente)\n");
    }
}

static void build_mount_table(void)
{
    int ext4_rc = -1;

    if (blk_is_ready())
        ext4_rc = ext4_mount();
    else
        ext4_unmount();

    build_dynamic_texts();

    vfs_mount_count = 0;
    mount_register("/", "bootstrap-root", "initrd-ro",
                   true, &rootfs_ops, 0U);
    mount_register("/dev", "device-nodes", "devfs",
                   false, &devfs_ops, 0U);

    if (blk_is_ready()) {
        if (ext4_rc == 0 && ext4_is_mounted()) {
            mount_register("/data", "data-root", "ext4",
                           false, ext4_vfs_ops(), 0U);
            mount_register("/sysroot", "sysroot-root", "ext4",
                           false, ext4_vfs_ops(), 0U);
        } else {
            mount_register("/data", "data-root", "ext4-error",
                           true, &placefs_ops, (uintptr_t)ext4_status());
            mount_register("/sysroot", "sysroot-root", "ext4-error",
                           true, &placefs_ops, (uintptr_t)ext4_status());
        }
    }
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

int vfs_open(const char *path, uint32_t flags, vfs_file_t *out)
{
    const vfs_mount_t *mount;

    if (!path || !out) return -EFAULT;
    if (path[0] != '/') return -ENOENT;

    mount = find_mount(path);
    if (!mount || !mount->ops || !mount->ops->open)
        return -ENOENT;

    return mount->ops->open(mount, relpath_from_mount(mount, path), flags, out);
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

    if (!path) return -EFAULT;
    if (path[0] != '/') return -ENOENT;

    mount = find_mount(path);
    if (!mount || !mount->ops || !mount->ops->mkdir)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->mkdir(mount, relpath_from_mount(mount, path), mode);
}

int vfs_unlink(const char *path)
{
    const vfs_mount_t *mount;

    if (!path) return -EFAULT;
    if (path[0] != '/') return -ENOENT;

    mount = find_mount(path);
    if (!mount || !mount->ops || !mount->ops->unlink)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->unlink(mount, relpath_from_mount(mount, path));
}

int vfs_rename(const char *old_path, const char *new_path)
{
    const vfs_mount_t *old_mount;
    const vfs_mount_t *new_mount;

    if (!old_path || !new_path) return -EFAULT;
    if (old_path[0] != '/' || new_path[0] != '/') return -ENOENT;

    old_mount = find_mount(old_path);
    new_mount = find_mount(new_path);
    if (!old_mount || !new_mount)
        return -ENOENT;
    if (old_mount != new_mount)
        return -EXDEV;
    if (!old_mount->ops || !old_mount->ops->rename)
        return old_mount->readonly ? -EROFS : -ENOSYS;

    return old_mount->ops->rename(old_mount, relpath_from_mount(old_mount, old_path),
                                  new_mount, relpath_from_mount(new_mount, new_path));
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

    if (!path) return -EFAULT;
    if (path[0] != '/') return -ENOENT;

    mount = find_mount(path);
    if (!mount || !mount->ops || !mount->ops->truncate)
        return mount && mount->readonly ? -EROFS : -ENOSYS;

    return mount->ops->truncate(mount, relpath_from_mount(mount, path), size);
}

int vfs_sync(void)
{
    int first_err = 0;

    for (size_t i = 0; i < vfs_mount_count; i++) {
        const vfs_mount_t *mount = &vfs_mounts[i];
        int rc;

        if (!mount->active || !mount->ops || !mount->ops->sync)
            continue;

        rc = mount->ops->sync(mount);
        if (rc < 0 && first_err == 0)
            first_err = rc;
    }

    return first_err;
}
