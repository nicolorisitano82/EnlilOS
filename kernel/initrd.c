/*
 * EnlilOS Microkernel - Embedded initrd CPIO rootfs (M5-05)
 *
 * Parser minimale del formato "newc" sufficiente per un initrd
 * read-only embedded nel kernel. Supporta file regolari e directory.
 */

#include "initrd.h"

#define INITRD_MAX_ENTRIES   64U
#define INITRD_ROOT_NODE     1U
#define INITRD_PARENT_ROOT   0xFFFFU
#define INITRD_STATUS_MAX    128U
#define CPIO_HEADER_LEN      110U

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t mode;
    uint32_t size;
    uint16_t parent;
    uint16_t _reserved;
    uint16_t name_len;
    uint8_t  valid;
} initrd_entry_t;

extern const uint8_t _binary_boot_initrd_cpio_start[];
extern const uint8_t _binary_boot_initrd_cpio_end[];

static initrd_entry_t initrd_entries[INITRD_MAX_ENTRIES];
static uint32_t       initrd_entry_count;
static uint8_t        initrd_ready;
static uint8_t        initrd_attempted;
static char           initrd_status_buf[INITRD_STATUS_MAX];

static size_t initrd_strlen(const char *s)
{
    size_t n = 0U;
    if (!s) return 0U;
    while (s[n] != '\0')
        n++;
    return n;
}

static void initrd_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- > 0U)
        *d++ = *s++;
}

static int initrd_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b);
}

static int initrd_nameeq(const char *a, size_t a_len, const char *b)
{
    size_t i;

    for (i = 0U; i < a_len && b[i] != '\0'; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return (i == a_len && b[i] == '\0');
}

static int initrd_prefixeq(const char *a, const char *b, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static uint32_t initrd_align4(uint32_t v)
{
    return (v + 3U) & ~3U;
}

static int initrd_hex_value(char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return -1;
}

static int initrd_parse_hex(const char *src, uint32_t digits, uint32_t *out)
{
    uint32_t value = 0U;

    if (!src || !out) return -1;
    for (uint32_t i = 0U; i < digits; i++) {
        int nibble = initrd_hex_value(src[i]);
        if (nibble < 0)
            return -1;
        value = (value << 4U) | (uint32_t)nibble;
    }
    *out = value;
    return 0;
}

static void initrd_status_set(const char *msg)
{
    size_t i = 0U;

    if (!msg) msg = "initrd status sconosciuto";
    while (msg[i] != '\0' && i + 1U < sizeof(initrd_status_buf)) {
        initrd_status_buf[i] = msg[i];
        i++;
    }
    initrd_status_buf[i] = '\0';
}

static char *initrd_buf_puts(char *dst, const char *src, char *end)
{
    while (dst < end && *src != '\0')
        *dst++ = *src++;
    if (dst < end)
        *dst = '\0';
    else
        end[-1] = '\0';
    return dst;
}

static char *initrd_buf_put_u32(char *dst, uint32_t value, char *end)
{
    char buf[10];
    uint32_t len = 0U;

    if (dst >= end)
        return dst;
    if (value == 0U) {
        if (dst + 1 < end) {
            *dst++ = '0';
            *dst = '\0';
        }
        return dst;
    }

    while (value != 0U && len < (uint32_t)sizeof(buf)) {
        buf[len++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (len > 0U && dst + 1 < end)
        *dst++ = buf[--len];
    *dst = '\0';
    return dst;
}

static void initrd_status_ok(uint32_t entry_count)
{
    char *p = initrd_status_buf;
    char *end = initrd_status_buf + sizeof(initrd_status_buf);

    p = initrd_buf_puts(p, "initrd cpio pronto (entries=", end);
    p = initrd_buf_put_u32(p, entry_count, end);
    p = initrd_buf_puts(p, ", bytes=", end);
    p = initrd_buf_put_u32(p,
                           (uint32_t)(_binary_boot_initrd_cpio_end -
                                      _binary_boot_initrd_cpio_start),
                           end);
    (void)initrd_buf_puts(p, ")", end);
}

static int initrd_find_entry_exact(const char *path, size_t path_len)
{
    for (uint32_t i = 0U; i < initrd_entry_count; i++) {
        if (!initrd_entries[i].valid)
            continue;
        if (initrd_entries[i].name_len != (uint16_t)path_len)
            continue;
        if (initrd_nameeq(path, path_len, initrd_entries[i].name))
            return (int)i;
    }
    return -1;
}

static int initrd_find_path(const char *relpath)
{
    const char *path = relpath;
    size_t      len;

    if (!path) return -1;
    while (*path == '/')
        path++;
    len = initrd_strlen(path);
    if (len == 0U)
        return -2;
    return initrd_find_entry_exact(path, len);
}

static int initrd_validate_name(const char *name, uint32_t name_len)
{
    if (!name || name_len == 0U)
        return -1;
    if (name[0] == '/')
        return -1;

    for (uint32_t i = 0U; i < name_len; i++) {
        if (name[i] == '\0')
            return -1;
        if (name[i] == '/' && (i == 0U || i + 1U == name_len))
            return -1;
    }
    return 0;
}

static int initrd_parent_from_name(const char *name, uint32_t name_len)
{
    uint32_t slash = name_len;
    uint8_t  found = 0U;

    while (slash > 0U) {
        slash--;
        if (name[slash] == '/') {
            found = 1U;
            break;
        }
    }

    if (!found)
        return (int)INITRD_PARENT_ROOT;
    if (slash == 0U)
        return -1;
    return initrd_find_entry_exact(name, slash);
}

static int initrd_parse_archive(void)
{
    const uint8_t *blob = _binary_boot_initrd_cpio_start;
    const uint8_t *end  = _binary_boot_initrd_cpio_end;
    const uint8_t *p    = blob;
    uint8_t        saw_trailer = 0U;

    initrd_entry_count = 0U;

    while (p + CPIO_HEADER_LEN <= end) {
        const char *hdr = (const char *)p;
        const char *name;
        const uint8_t *data;
        uint32_t mode;
        uint32_t filesize;
        uint32_t namesize;
        uint32_t name_len;
        uint32_t data_off;
        int      parent;

        if (!initrd_prefixeq(hdr, "070701", 6U)) {
            initrd_status_set("initrd: magic CPIO non valido");
            return -1;
        }
        if (initrd_parse_hex(hdr + 14, 8U, &mode) < 0 ||
            initrd_parse_hex(hdr + 54, 8U, &filesize) < 0 ||
            initrd_parse_hex(hdr + 94, 8U, &namesize) < 0) {
            initrd_status_set("initrd: header CPIO corrotto");
            return -1;
        }
        if (namesize == 0U || p + CPIO_HEADER_LEN + namesize > end) {
            initrd_status_set("initrd: namesize non valido");
            return -1;
        }

        name = (const char *)(p + CPIO_HEADER_LEN);
        if (name[namesize - 1U] != '\0') {
            initrd_status_set("initrd: nome entry non terminato");
            return -1;
        }
        name_len = namesize - 1U;
        data_off = initrd_align4(CPIO_HEADER_LEN + namesize);
        if (p + data_off + filesize > end) {
            initrd_status_set("initrd: file oltre fine archivio");
            return -1;
        }

        if (initrd_streq(name, "TRAILER!!!")) {
            saw_trailer = 1U;
            break;
        }
        if (initrd_entry_count >= INITRD_MAX_ENTRIES) {
            initrd_status_set("initrd: troppe entry");
            return -1;
        }
        if (initrd_validate_name(name, name_len) < 0) {
            initrd_status_set("initrd: path entry non valido");
            return -1;
        }

        parent = initrd_parent_from_name(name, name_len);
        if (parent < 0) {
            initrd_status_set("initrd: parent directory mancante");
            return -1;
        }
        if (parent != (int)INITRD_PARENT_ROOT &&
            (initrd_entries[parent].mode & S_IFMT) != S_IFDIR) {
            initrd_status_set("initrd: parent non directory");
            return -1;
        }

        data = p + data_off;
        initrd_entries[initrd_entry_count].name     = name;
        initrd_entries[initrd_entry_count].data     = data;
        initrd_entries[initrd_entry_count].mode     = mode;
        initrd_entries[initrd_entry_count].size     = filesize;
        initrd_entries[initrd_entry_count].parent   = (uint16_t)parent;
        initrd_entries[initrd_entry_count].name_len = (uint16_t)name_len;
        initrd_entries[initrd_entry_count].valid    = 1U;
        initrd_entry_count++;

        p += initrd_align4(data_off + filesize);
    }

    if (!saw_trailer) {
        initrd_status_set("initrd: trailer CPIO assente");
        return -1;
    }

    initrd_ready = 1U;
    initrd_status_ok(initrd_entry_count);
    return 0;
}

static void initrd_file_reset(vfs_file_t *file, const vfs_mount_t *mount,
                              uint32_t node_id, uint32_t flags)
{
    file->mount     = mount;
    file->node_id   = node_id;
    file->flags     = flags;
    file->pos       = 0U;
    file->size_hint = 0U;
    file->dir_index = 0U;
    file->cookie    = 0U;
}

static void initrd_file_bind(vfs_file_t *file, const vfs_mount_t *mount,
                             uint32_t node_id, uint32_t flags, uint32_t index)
{
    initrd_entry_t *entry = &initrd_entries[index];

    initrd_file_reset(file, mount, node_id, flags);
    file->cookie    = (uintptr_t)(index + 1U);
    file->size_hint = entry->size;
}

static int initrd_wants_write(uint32_t flags)
{
    uint32_t access = (flags & 0x3U);

    return (access == O_WRONLY || access == O_RDWR ||
            (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0U);
}

static int initrd_open(const vfs_mount_t *mount, const char *relpath,
                       uint32_t flags, vfs_file_t *out)
{
    int idx;

    if (!out) return -EFAULT;
    if (!initrd_ready) return -EIO;
    if (initrd_wants_write(flags))
        return -EROFS;

    idx = initrd_find_path(relpath);
    if (idx == -2) {
        initrd_file_reset(out, mount, INITRD_ROOT_NODE, flags);
        return 0;
    }
    if (idx < 0)
        return -ENOENT;

    initrd_file_bind(out, mount, (uint32_t)idx + 2U, flags, (uint32_t)idx);
    return 0;
}

static ssize_t initrd_read(vfs_file_t *file, void *buf, size_t count)
{
    initrd_entry_t *entry;
    size_t          remain;

    if (!file || !buf) return -EFAULT;
    if (file->node_id == INITRD_ROOT_NODE)
        return -EISDIR;
    if (file->cookie == 0U)
        return -EBADF;

    entry = &initrd_entries[(uint32_t)file->cookie - 1U];
    if ((entry->mode & S_IFMT) == S_IFDIR)
        return -EISDIR;
    if (file->pos >= entry->size)
        return 0;

    remain = (size_t)(entry->size - file->pos);
    if (count > remain)
        count = remain;
    initrd_memcpy(buf, entry->data + file->pos, count);
    file->pos += count;
    return (ssize_t)count;
}

static ssize_t initrd_write(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    return -EROFS;
}

static int initrd_dirent_fill(vfs_dirent_t *out, const char *name, uint32_t mode)
{
    size_t i = 0U;

    if (!out || !name) return -EFAULT;
    while (name[i] != '\0' && i + 1U < sizeof(out->name)) {
        out->name[i] = name[i];
        i++;
    }
    out->name[i] = '\0';
    out->mode = mode;
    return 0;
}

static int initrd_child_name(const initrd_entry_t *entry, const char **name_out)
{
    uint32_t i = entry->name_len;

    if (!entry || !name_out) return -1;
    while (i > 0U) {
        i--;
        if (entry->name[i] == '/') {
            *name_out = entry->name + i + 1U;
            return 0;
        }
    }
    *name_out = entry->name;
    return 0;
}

static int initrd_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    uint16_t parent = INITRD_PARENT_ROOT;

    if (!file || !out) return -EFAULT;
    if (file->node_id != INITRD_ROOT_NODE) {
        initrd_entry_t *entry;

        if (file->cookie == 0U)
            return -EBADF;
        entry = &initrd_entries[(uint32_t)file->cookie - 1U];
        if ((entry->mode & S_IFMT) != S_IFDIR)
            return -ENOTDIR;
        parent = (uint16_t)((uint32_t)file->cookie - 1U);
    }

    while (file->dir_index < initrd_entry_count) {
        const initrd_entry_t *entry = &initrd_entries[file->dir_index++];
        const char           *name;

        if (!entry->valid || entry->parent != parent)
            continue;
        (void)initrd_child_name(entry, &name);
        return initrd_dirent_fill(out, name, entry->mode);
    }

    return -ENOENT;
}

static int initrd_stat(vfs_file_t *file, stat_t *out)
{
    initrd_entry_t *entry;

    if (!file || !out) return -EFAULT;

    if (file->node_id == INITRD_ROOT_NODE) {
        out->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        out->st_blksize = 512U;
        out->st_size = 0U;
        out->st_blocks = 0U;
        return 0;
    }
    if (file->cookie == 0U)
        return -EBADF;

    entry = &initrd_entries[(uint32_t)file->cookie - 1U];
    out->st_mode = entry->mode;
    out->st_blksize = 512U;
    out->st_size = entry->size;
    out->st_blocks = (entry->size + 511U) / 512U;
    return 0;
}

static int initrd_close(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static const vfs_ops_t initrd_ops = {
    .open    = initrd_open,
    .read    = initrd_read,
    .write   = initrd_write,
    .readdir = initrd_readdir,
    .stat    = initrd_stat,
    .close   = initrd_close,
};

int initrd_init(void)
{
    if (initrd_attempted)
        return initrd_ready ? 0 : -1;

    initrd_attempted = 1U;
    initrd_ready = 0U;
    return initrd_parse_archive();
}

int initrd_is_ready(void)
{
    return initrd_ready ? 1 : 0;
}

const char *initrd_status(void)
{
    if (initrd_status_buf[0] == '\0')
        return "initrd non inizializzato";
    return initrd_status_buf;
}

const vfs_ops_t *initrd_vfs_ops(void)
{
    return &initrd_ops;
}
