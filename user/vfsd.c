/*
 * EnlilOS vfsd - namespace aware VFS server (M9-04 v1)
 *
 * Il backend filesystem resta bootstrap in-kernel, ma la risoluzione dei path,
 * il cwd, le mount table dinamiche e i namespace mount privati vivono gia'
 * in user-space.
 */

#include "microkernel.h"
#include "syscall.h"
#include "user_svc.h"
#include "vfs_ipc.h"

typedef unsigned long  u64;
typedef signed long    s64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define VFSD_MAX_CLIENTS      64U
#define VFSD_MAX_NAMESPACES   32U
#define VFSD_MAX_NS_MOUNTS    16U
#define VFSD_ROOT_NS_ID       1U

typedef struct {
    u8   active;
    u8   readonly;
    u8   linux_compat;
    u8   _pad0;
    char mount_at[VFSD_PATH_BYTES];
    char backend[VFSD_PATH_BYTES];
} vfsd_ns_mount_t;

typedef struct {
    u8              in_use;
    u8              _pad0[3];
    u32             ns_id;
    vfsd_ns_mount_t mounts[VFSD_MAX_NS_MOUNTS];
} vfsd_namespace_t;

typedef struct {
    u8   in_use;
    u8   _pad0[3];
    u32  tgid;
    u32  parent_pid;
    u32  ns_id;
    char cwd[VFSD_IO_BYTES];
} vfsd_client_t;

static vfsd_namespace_t g_namespaces[VFSD_MAX_NAMESPACES];
static vfsd_client_t    g_clients[VFSD_MAX_CLIENTS];
static u32              g_next_ns_id = VFSD_ROOT_NS_ID + 1U;

static int vfsd_boot_path_stat(const char *path, stat_t *st);

static long sys_call1(long nr, long a0)
{
    return user_svc1(nr, a0);
}

static long sys_call2(long nr, long a0, long a1)
{
    return user_svc2(nr, a0, a1);
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    return user_svc3(nr, a0, a1, a2);
}

static long sys_call4(long nr, long a0, long a1, long a2, long a3)
{
    return user_svc4(nr, a0, a1, a2, a3);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

static u32 vfsd_strlen(const char *s)
{
    u32 n = 0U;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static int vfsd_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b);
}

static void vfsd_bzero(void *ptr, u64 len)
{
    u8 *p = (u8 *)ptr;
    while (len-- > 0U)
        *p++ = 0U;
}

static void vfsd_memcpy(void *dst, const void *src, u64 len)
{
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (len-- > 0U)
        *d++ = *s++;
}

static void vfsd_strlcpy(char *dst, const char *src, u32 cap)
{
    u32 i = 0U;

    if (!dst || cap == 0U)
        return;
    while (src && src[i] != '\0' && i + 1U < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void vfsd_puts(const char *s)
{
    (void)sys_call3(SYS_WRITE, 1, (long)s, (long)vfsd_strlen(s));
}

static long sys_port_lookup_name(const char *name)
{
    return sys_call1(SYS_PORT_LOOKUP, (long)name);
}

static long sys_ipc_wait_msg(u32 port_id, ipc_message_t *msg)
{
    return sys_call2(SYS_IPC_WAIT, (long)port_id, (long)msg);
}

static long sys_ipc_reply_msg(u32 port_id, u32 type, const void *buf, u32 len)
{
    return sys_call4(SYS_IPC_REPLY, (long)port_id, (long)type, (long)buf, (long)len);
}

static long sys_vfs_boot_open_now(const char *path, u32 flags)
{
    return sys_call2(SYS_VFS_BOOT_OPEN, (long)path, (long)flags);
}

static long sys_vfs_boot_read_now(u32 handle, void *buf, u32 len)
{
    return sys_call3(SYS_VFS_BOOT_READ, (long)handle, (long)buf, (long)len);
}

static long sys_vfs_boot_write_now(u32 handle, const void *buf, u32 len)
{
    return sys_call3(SYS_VFS_BOOT_WRITE, (long)handle, (long)buf, (long)len);
}

static long sys_vfs_boot_readdir_now(u32 handle, vfs_dirent_t *ent)
{
    return sys_call2(SYS_VFS_BOOT_READDIR, (long)handle, (long)ent);
}

static long sys_vfs_boot_stat_now(u32 handle, stat_t *st)
{
    return sys_call2(SYS_VFS_BOOT_STAT, (long)handle, (long)st);
}

static long sys_vfs_boot_close_now(u32 handle)
{
    return sys_call1(SYS_VFS_BOOT_CLOSE, (long)handle);
}

static long sys_vfs_boot_lseek_now(u32 handle, s64 offset, u32 whence)
{
    return sys_call3(SYS_VFS_BOOT_LSEEK, (long)handle, (long)offset, (long)whence);
}

static long sys_vfs_boot_taskinfo_now(u32 pid, vfsd_taskinfo_t *info)
{
    return sys_call2(SYS_VFS_BOOT_TASKINFO, (long)pid, (long)info);
}

static void vfsd_reply_error(u32 port_id, long rc)
{
    vfsd_response_t resp;

    vfsd_bzero(&resp, sizeof(resp));
    resp.status = (int)rc;
    (void)sys_ipc_reply_msg(port_id, IPC_MSG_VFS_RESP, &resp, (u32)sizeof(resp));
}

static int vfsd_path_has_prefix(const char *path, const char *prefix)
{
    u32 i = 0U;

    if (!path || !prefix)
        return 0;
    if (prefix[0] == '/' && prefix[1] == '\0')
        return path[0] == '/';

    while (prefix[i] != '\0') {
        if (path[i] != prefix[i])
            return 0;
        i++;
    }

    return path[i] == '\0' || path[i] == '/';
}

static void vfsd_path_pop(char *path)
{
    u32 len = vfsd_strlen(path);

    if (len <= 1U) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    while (len > 1U && path[len - 1U] == '/')
        path[--len] = '\0';
    while (len > 1U && path[len - 1U] != '/')
        path[--len] = '\0';
    if (len > 1U)
        path[len - 1U] = '\0';
    if (path[0] == '\0') {
        path[0] = '/';
        path[1] = '\0';
    }
}

static int vfsd_path_append_seg(char *out, u32 cap, const char *seg, u32 seg_len)
{
    u32 len = vfsd_strlen(out);

    if (seg_len == 0U)
        return 0;
    if (len == 0U || out[0] != '/')
        return -EINVAL;
    if (len > 1U) {
        if (len + 1U >= cap)
            return -ENAMETOOLONG;
        out[len++] = '/';
    }
    if (len + seg_len >= cap)
        return -ENAMETOOLONG;

    for (u32 i = 0U; i < seg_len; i++)
        out[len + i] = seg[i];
    out[len + seg_len] = '\0';
    return 0;
}

static int vfsd_path_normalize(const char *base, const char *input,
                               char *out, u32 cap)
{
    char tmp[VFSD_IO_BYTES];
    u32  len = 0U;
    u32  i = 0U;

    if (!input || !out || cap < 2U)
        return -EINVAL;

    vfsd_bzero(tmp, sizeof(tmp));
    if (input[0] == '/') {
        vfsd_strlcpy(tmp, input, sizeof(tmp));
    } else {
        const char *root = (base && base[0] == '/') ? base : "/";
        u32 base_len = vfsd_strlen(root);

        if (base_len + 1U >= sizeof(tmp))
            return -ENAMETOOLONG;
        vfsd_strlcpy(tmp, root, sizeof(tmp));
        len = vfsd_strlen(tmp);
        if (len > 1U && tmp[len - 1U] != '/')
            tmp[len++] = '/';
        for (i = 0U; input[i] != '\0' && len + 1U < sizeof(tmp); i++)
            tmp[len++] = input[i];
        if (input[i] != '\0')
            return -ENAMETOOLONG;
        tmp[len] = '\0';
    }

    out[0] = '/';
    out[1] = '\0';
    i = 0U;
    while (tmp[i] != '\0') {
        u32 seg_start;
        u32 seg_len;

        while (tmp[i] == '/')
            i++;
        if (tmp[i] == '\0')
            break;

        seg_start = i;
        while (tmp[i] != '\0' && tmp[i] != '/')
            i++;
        seg_len = i - seg_start;

        if (seg_len == 1U && tmp[seg_start] == '.')
            continue;
        if (seg_len == 2U && tmp[seg_start] == '.' && tmp[seg_start + 1U] == '.') {
            vfsd_path_pop(out);
            continue;
        }

        if (vfsd_path_append_seg(out, cap, &tmp[seg_start], seg_len) < 0)
            return -ENAMETOOLONG;
    }

    return 0;
}

static int vfsd_path_join(const char *prefix, const char *suffix,
                          char *out, u32 cap)
{
    u32 p_len;
    u32 s_off = 0U;
    u32 i;

    if (!prefix || !suffix || !out || cap < 2U)
        return -EINVAL;

    p_len = vfsd_strlen(prefix);
    if (p_len == 0U)
        return -EINVAL;

    if (prefix[0] == '/' && prefix[1] == '\0')
        p_len = 1U;
    while (suffix[s_off] == '/')
        s_off++;

    if (prefix[0] == '/' && prefix[1] == '\0') {
        out[0] = '/';
        out[1] = '\0';
        if (suffix[s_off] == '\0')
            return 0;
        if (1U + vfsd_strlen(&suffix[s_off]) >= cap)
            return -ENAMETOOLONG;
        for (i = 0U; suffix[s_off + i] != '\0'; i++)
            out[1U + i] = suffix[s_off + i];
        out[1U + i] = '\0';
        return 0;
    }

    if (p_len + 1U >= cap)
        return -ENAMETOOLONG;
    vfsd_strlcpy(out, prefix, cap);
    if (suffix[s_off] == '\0')
        return 0;

    if (out[p_len - 1U] != '/') {
        out[p_len++] = '/';
        out[p_len] = '\0';
    }

    if (p_len + vfsd_strlen(&suffix[s_off]) >= cap)
        return -ENAMETOOLONG;
    for (i = 0U; suffix[s_off + i] != '\0'; i++)
        out[p_len + i] = suffix[s_off + i];
    out[p_len + i] = '\0';
    return 0;
}

static int vfsd_path_rebase_under(const char *root, const char *path,
                                  char *out, u32 cap)
{
    const char *suffix;

    if (!root || !path || !out || cap < 2U)
        return -EINVAL;
    if (!vfsd_path_has_prefix(path, root))
        return -EINVAL;
    if (root[0] == '/' && root[1] == '\0') {
        vfsd_strlcpy(out, path, cap);
        return 0;
    }

    suffix = path + vfsd_strlen(root);
    if (suffix[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    return vfsd_path_join("/", suffix, out, cap);
}

static u8 vfsd_backend_readonly(const char *backend)
{
    if (!backend)
        return 0U;
    if (backend[0] == '/' && backend[1] == '\0')
        return 1U;
    if (vfsd_streq(backend, "/proc"))
        return 1U;
    return 0U;
}

static vfsd_namespace_t *vfsd_namespace_find(u32 ns_id)
{
    for (u32 i = 0U; i < VFSD_MAX_NAMESPACES; i++) {
        if (g_namespaces[i].in_use && g_namespaces[i].ns_id == ns_id)
            return &g_namespaces[i];
    }
    return NULL;
}

static void vfsd_namespace_seed(vfsd_namespace_t *ns)
{
    if (!ns)
        return;

    vfsd_bzero(ns, sizeof(*ns));
    ns->in_use = 1U;
    if (ns->ns_id == 0U)
        ns->ns_id = VFSD_ROOT_NS_ID;

    ns->mounts[0].active = 1U;
    ns->mounts[0].readonly = 1U;
    vfsd_strlcpy(ns->mounts[0].mount_at, "/", sizeof(ns->mounts[0].mount_at));
    vfsd_strlcpy(ns->mounts[0].backend, "/", sizeof(ns->mounts[0].backend));

    ns->mounts[1].active = 1U;
    ns->mounts[1].readonly = 0U;
    vfsd_strlcpy(ns->mounts[1].mount_at, "/dev", sizeof(ns->mounts[1].mount_at));
    vfsd_strlcpy(ns->mounts[1].backend, "/dev", sizeof(ns->mounts[1].backend));

    ns->mounts[2].active = 1U;
    ns->mounts[2].readonly = 1U;
    vfsd_strlcpy(ns->mounts[2].mount_at, "/proc", sizeof(ns->mounts[2].mount_at));
    vfsd_strlcpy(ns->mounts[2].backend, "/proc", sizeof(ns->mounts[2].backend));

    ns->mounts[3].active = 1U;
    ns->mounts[3].readonly = 0U;
    vfsd_strlcpy(ns->mounts[3].mount_at, "/data", sizeof(ns->mounts[3].mount_at));
    vfsd_strlcpy(ns->mounts[3].backend, "/data", sizeof(ns->mounts[3].backend));

    ns->mounts[4].active = 1U;
    ns->mounts[4].readonly = 0U;
    vfsd_strlcpy(ns->mounts[4].mount_at, "/sysroot", sizeof(ns->mounts[4].mount_at));
    vfsd_strlcpy(ns->mounts[4].backend, "/sysroot", sizeof(ns->mounts[4].backend));

    ns->mounts[5].active = 1U;
    ns->mounts[5].readonly = 1U;
    ns->mounts[5].linux_compat = 1U;
    vfsd_strlcpy(ns->mounts[5].mount_at, "/lib", sizeof(ns->mounts[5].mount_at));
    vfsd_strlcpy(ns->mounts[5].backend, "/sysroot/lib", sizeof(ns->mounts[5].backend));

    ns->mounts[6].active = 1U;
    ns->mounts[6].readonly = 1U;
    ns->mounts[6].linux_compat = 1U;
    vfsd_strlcpy(ns->mounts[6].mount_at, "/usr", sizeof(ns->mounts[6].mount_at));
    vfsd_strlcpy(ns->mounts[6].backend, "/sysroot/usr", sizeof(ns->mounts[6].backend));

    ns->mounts[7].active = 1U;
    ns->mounts[7].readonly = 0U;
    vfsd_strlcpy(ns->mounts[7].mount_at, "/var", sizeof(ns->mounts[7].mount_at));
    vfsd_strlcpy(ns->mounts[7].backend, "/data/var", sizeof(ns->mounts[7].backend));

    ns->mounts[8].active = 1U;
    ns->mounts[8].readonly = 0U;
    vfsd_strlcpy(ns->mounts[8].mount_at, "/tmp", sizeof(ns->mounts[8].mount_at));
    vfsd_strlcpy(ns->mounts[8].backend, "/data/tmp", sizeof(ns->mounts[8].backend));

    ns->mounts[9].active = 1U;
    ns->mounts[9].readonly = 1U;
    ns->mounts[9].linux_compat = 1U;
    vfsd_strlcpy(ns->mounts[9].mount_at, "/bin/sh", sizeof(ns->mounts[9].mount_at));
    vfsd_strlcpy(ns->mounts[9].backend, "/sysroot/usr/bin/bash", sizeof(ns->mounts[9].backend));
}

static void vfsd_namespace_init(void)
{
    vfsd_bzero(g_namespaces, sizeof(g_namespaces));
    g_namespaces[0].ns_id = VFSD_ROOT_NS_ID;
    vfsd_namespace_seed(&g_namespaces[0]);
}

static vfsd_namespace_t *vfsd_namespace_alloc(void)
{
    for (u32 i = 0U; i < VFSD_MAX_NAMESPACES; i++) {
        if (g_namespaces[i].in_use)
            continue;
        g_namespaces[i].ns_id = g_next_ns_id++;
        g_namespaces[i].in_use = 1U;
        return &g_namespaces[i];
    }
    return NULL;
}

static vfsd_namespace_t *vfsd_namespace_clone(u32 src_ns_id)
{
    vfsd_namespace_t *src = vfsd_namespace_find(src_ns_id);
    vfsd_namespace_t *dst = vfsd_namespace_alloc();

    if (!src || !dst)
        return NULL;

    vfsd_memcpy(dst, src, sizeof(*dst));
    dst->in_use = 1U;
    dst->ns_id = g_next_ns_id - 1U;
    return dst;
}

static vfsd_ns_mount_t *vfsd_namespace_mount_exact(vfsd_namespace_t *ns, const char *mount_at)
{
    if (!ns || !mount_at)
        return NULL;

    for (u32 i = 0U; i < VFSD_MAX_NS_MOUNTS; i++) {
        if (ns->mounts[i].active && vfsd_streq(ns->mounts[i].mount_at, mount_at))
            return &ns->mounts[i];
    }
    return NULL;
}

static vfsd_ns_mount_t *vfsd_namespace_mount_best(vfsd_namespace_t *ns, const char *path)
{
    vfsd_ns_mount_t *best = NULL;
    u32              best_len = 0U;

    if (!ns || !path)
        return NULL;

    for (u32 i = 0U; i < VFSD_MAX_NS_MOUNTS; i++) {
        u32 len;

        if (!ns->mounts[i].active)
            continue;
        if (!vfsd_path_has_prefix(path, ns->mounts[i].mount_at))
            continue;

        len = vfsd_strlen(ns->mounts[i].mount_at);
        if (!best || len > best_len) {
            best = &ns->mounts[i];
            best_len = len;
        }
    }

    return best;
}

static vfsd_ns_mount_t *vfsd_namespace_mount_best_excluding(vfsd_namespace_t *ns,
                                                            const char *path,
                                                            const vfsd_ns_mount_t *exclude)
{
    vfsd_ns_mount_t *best = NULL;
    u32              best_len = 0U;

    if (!ns || !path)
        return NULL;

    for (u32 i = 0U; i < VFSD_MAX_NS_MOUNTS; i++) {
        u32 len;

        if (!ns->mounts[i].active || &ns->mounts[i] == exclude)
            continue;
        if (!vfsd_path_has_prefix(path, ns->mounts[i].mount_at))
            continue;

        len = vfsd_strlen(ns->mounts[i].mount_at);
        if (!best || len > best_len) {
            best = &ns->mounts[i];
            best_len = len;
        }
    }

    return best;
}

static int vfsd_namespace_mount_join(const vfsd_ns_mount_t *mount,
                                     const char *virtual_path,
                                     char *backend_out,
                                     u8 *readonly_out,
                                     u8 *linux_compat_out)
{
    const char *suffix;

    if (!mount || !virtual_path || !backend_out)
        return -EINVAL;

    suffix = virtual_path;
    if (!(mount->mount_at[0] == '/' && mount->mount_at[1] == '\0'))
        suffix += vfsd_strlen(mount->mount_at);

    if (vfsd_path_join(mount->backend, suffix, backend_out, VFSD_PATH_BYTES) < 0)
        return -ENAMETOOLONG;
    if (readonly_out)
        *readonly_out = mount->readonly;
    if (linux_compat_out)
        *linux_compat_out = mount->linux_compat;
    return 0;
}

static int vfsd_namespace_resolve_existing(vfsd_namespace_t *ns, const char *virtual_path,
                                           char *backend_out, u8 *readonly_out,
                                           u8 *linux_compat_out)
{
    vfsd_ns_mount_t *mount;
    stat_t           st;
    int              rc = -ENOENT;

    if (!ns || !virtual_path || !backend_out)
        return -EINVAL;

    mount = vfsd_namespace_mount_best(ns, virtual_path);
    while (mount) {
        rc = vfsd_namespace_mount_join(mount, virtual_path, backend_out,
                                       readonly_out, linux_compat_out);
        if (rc < 0)
            return rc;
        if (!mount->linux_compat)
            return 0;
        rc = vfsd_boot_path_stat(backend_out, &st);
        if (rc == 0)
            return 0;
        if (rc != -ENOENT && rc != -ENOTDIR)
            return rc;
        mount = vfsd_namespace_mount_best_excluding(ns, virtual_path, mount);
    }

    return rc;
}

static int vfsd_namespace_open_existing(vfsd_namespace_t *ns, const char *virtual_path,
                                        u32 flags, char *backend_out)
{
    vfsd_ns_mount_t *mount;
    int              rc = -ENOENT;

    if (!ns || !virtual_path)
        return -EINVAL;

    mount = vfsd_namespace_mount_best(ns, virtual_path);
    while (mount) {
        char backend[VFSD_PATH_BYTES];

        rc = vfsd_namespace_mount_join(mount, virtual_path, backend, NULL, NULL);
        if (rc < 0)
            return rc;

        rc = (int)sys_vfs_boot_open_now(backend, flags);
        if (rc >= 0) {
            if (backend_out)
                vfsd_strlcpy(backend_out, backend, VFSD_PATH_BYTES);
            return rc;
        }

        if (!mount->linux_compat || (rc != -ENOENT && rc != -ENOTDIR))
            return rc;
        mount = vfsd_namespace_mount_best_excluding(ns, virtual_path, mount);
    }

    return rc;
}

static int vfsd_namespace_mount_add(vfsd_namespace_t *ns, const char *mount_at,
                                    const char *backend, u8 readonly,
                                    u8 linux_compat)
{
    if (!ns || !mount_at || !backend)
        return -EINVAL;
    if (vfsd_namespace_mount_exact(ns, mount_at))
        return -EBUSY;

    for (u32 i = 0U; i < VFSD_MAX_NS_MOUNTS; i++) {
        if (ns->mounts[i].active)
            continue;
        ns->mounts[i].active = 1U;
        ns->mounts[i].readonly = readonly;
        ns->mounts[i].linux_compat = linux_compat;
        vfsd_strlcpy(ns->mounts[i].mount_at, mount_at, sizeof(ns->mounts[i].mount_at));
        vfsd_strlcpy(ns->mounts[i].backend, backend, sizeof(ns->mounts[i].backend));
        return 0;
    }

    return -ENOSPC;
}

static int vfsd_namespace_umount(vfsd_namespace_t *ns, const char *mount_at)
{
    vfsd_ns_mount_t *mount;

    if (!ns || !mount_at)
        return -EINVAL;
    if (vfsd_streq(mount_at, "/"))
        return -EBUSY;

    for (u32 i = 0U; i < VFSD_MAX_NS_MOUNTS; i++) {
        if (!ns->mounts[i].active || vfsd_streq(ns->mounts[i].mount_at, mount_at))
            continue;
        if (vfsd_path_has_prefix(ns->mounts[i].mount_at, mount_at))
            return -EBUSY;
    }

    mount = vfsd_namespace_mount_exact(ns, mount_at);
    if (!mount)
        return -ENOENT;

    vfsd_bzero(mount, sizeof(*mount));
    return 0;
}

static int vfsd_namespace_resolve(vfsd_namespace_t *ns, const char *virtual_path,
                                  char *backend_out, u8 *readonly_out,
                                  u8 *linux_compat_out)
{
    vfsd_ns_mount_t *mount;

    if (!ns || !virtual_path || !backend_out)
        return -EINVAL;

    mount = vfsd_namespace_mount_best(ns, virtual_path);
    if (!mount)
        return -ENOENT;
    return vfsd_namespace_mount_join(mount, virtual_path, backend_out,
                                     readonly_out, linux_compat_out);
}

static vfsd_client_t *vfsd_client_find(u32 tgid)
{
    for (u32 i = 0U; i < VFSD_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].tgid == tgid)
            return &g_clients[i];
    }
    return NULL;
}

static vfsd_client_t *vfsd_client_alloc(void)
{
    for (u32 i = 0U; i < VFSD_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use)
            continue;
        g_clients[i].in_use = 1U;
        return &g_clients[i];
    }
    return NULL;
}

static vfsd_client_t *vfsd_client_get_or_create(u32 pid)
{
    vfsd_taskinfo_t  info;
    vfsd_client_t   *client;
    vfsd_client_t   *parent;

    client = vfsd_client_find(pid);
    if (client)
        return client;

    vfsd_bzero(&info, sizeof(info));
    if (sys_vfs_boot_taskinfo_now(pid, &info) < 0)
        return NULL;

    client = vfsd_client_find(info.tgid);
    if (client)
        return client;

    client = vfsd_client_alloc();
    if (!client)
        return NULL;

    vfsd_bzero(client, sizeof(*client));
    client->in_use = 1U;
    client->tgid = info.tgid;
    client->parent_pid = info.parent_pid;

    parent = vfsd_client_find(info.parent_pid);
    if (parent) {
        client->ns_id = parent->ns_id;
        vfsd_strlcpy(client->cwd, parent->cwd, sizeof(client->cwd));
    } else {
        client->ns_id = VFSD_ROOT_NS_ID;
        vfsd_strlcpy(client->cwd, "/", sizeof(client->cwd));
    }

    return client;
}

static int vfsd_boot_path_stat(const char *path, stat_t *st)
{
    long handle;
    long rc;

    if (!path || !st)
        return -EINVAL;

    handle = sys_vfs_boot_open_now(path, O_RDONLY);
    if (handle < 0)
        return (int)handle;

    rc = sys_vfs_boot_stat_now((u32)handle, st);
    (void)sys_vfs_boot_close_now((u32)handle);
    return (int)rc;
}

static int vfsd_client_virtual_path(vfsd_client_t *client, const char *input,
                                    char *virtual_out)
{
    if (!client || !input || !virtual_out)
        return -EINVAL;
    return vfsd_path_normalize(client->cwd, input, virtual_out, VFSD_IO_BYTES);
}

static int vfsd_client_backend_path(vfsd_client_t *client, const char *input,
                                    char *virtual_out, char *backend_out)
{
    vfsd_namespace_t *ns;

    if (!client || !input || !backend_out)
        return -EINVAL;

    if (vfsd_client_virtual_path(client, input, virtual_out) < 0)
        return -ENAMETOOLONG;

    ns = vfsd_namespace_find(client->ns_id);
    if (!ns)
        return -ENOENT;

    return vfsd_namespace_resolve_existing(ns, virtual_out, backend_out, NULL, NULL);
}

static int vfsd_mount_backend_for(vfsd_client_t *client, u32 fs_type, const char *src,
                                  char *backend_out, u8 *readonly_out)
{
    char  virtual_path[VFSD_IO_BYTES];
    stat_t st;
    int    rc;

    if (!client || !backend_out)
        return -EINVAL;

    switch (fs_type) {
    case VFSD_FS_BIND:
        if (!src || src[0] == '\0')
            return -EINVAL;
        rc = vfsd_client_backend_path(client, src, virtual_path, backend_out);
        if (rc < 0)
            return rc;
        rc = vfsd_boot_path_stat(backend_out, &st);
        if (rc < 0)
            return rc;
        if (readonly_out)
            *readonly_out = 0U;
        return 0;
    case VFSD_FS_INITRD:
        vfsd_strlcpy(backend_out, "/", VFSD_PATH_BYTES);
        if (readonly_out)
            *readonly_out = 1U;
        return 0;
    case VFSD_FS_DEVFS:
        vfsd_strlcpy(backend_out, "/dev", VFSD_PATH_BYTES);
        if (readonly_out)
            *readonly_out = 0U;
        return 0;
    case VFSD_FS_PROCFS:
        vfsd_strlcpy(backend_out, "/proc", VFSD_PATH_BYTES);
        if (readonly_out)
            *readonly_out = 1U;
        return 0;
    case VFSD_FS_EXT4_SYSROOT:
        vfsd_strlcpy(backend_out, "/sysroot", VFSD_PATH_BYTES);
        if (readonly_out)
            *readonly_out = 0U;
        return 0;
    case VFSD_FS_EXT4_DATA:
        if (src && src[0] != '\0' && vfsd_streq(src, "sysroot")) {
            vfsd_strlcpy(backend_out, "/sysroot", VFSD_PATH_BYTES);
        } else {
            vfsd_strlcpy(backend_out, "/data", VFSD_PATH_BYTES);
        }
        if (readonly_out)
            *readonly_out = 0U;
        return 0;
    default:
        return -EINVAL;
    }
}

static int vfsd_client_chdir(vfsd_client_t *client, const char *path)
{
    char   virtual_path[VFSD_IO_BYTES];
    char   backend[VFSD_PATH_BYTES];
    stat_t st;
    int    rc;
    vfsd_namespace_t *ns;

    if (!client)
        return -EINVAL;

    rc = vfsd_client_virtual_path(client, path, virtual_path);
    if (rc < 0)
        return rc;
    ns = vfsd_namespace_find(client->ns_id);
    if (!ns)
        return -ENOENT;
    rc = vfsd_namespace_resolve_existing(ns, virtual_path, backend, NULL, NULL);
    if (rc < 0)
        return rc;

    rc = vfsd_boot_path_stat(backend, &st);
    if (rc < 0)
        return rc;
    if ((st.st_mode & S_IFMT) != S_IFDIR)
        return -ENOTDIR;

    vfsd_strlcpy(client->cwd, virtual_path, sizeof(client->cwd));
    return 0;
}

static int vfsd_client_mount(vfsd_client_t *client, u32 fs_type, u32 flags,
                             const char *src, const char *target)
{
    vfsd_namespace_t *ns;
    char              target_virtual[VFSD_IO_BYTES];
    char              backend[VFSD_PATH_BYTES];
    u8                readonly = 0U;
    int               rc;

    if (!client || !target)
        return -EINVAL;

    rc = vfsd_path_normalize(client->cwd, target, target_virtual, sizeof(target_virtual));
    if (rc < 0)
        return rc;
    if (vfsd_streq(target_virtual, "/"))
        return -EBUSY;

    rc = vfsd_mount_backend_for(client, fs_type, src, backend, &readonly);
    if (rc < 0)
        return rc;
    if (flags & MS_RDONLY)
        readonly = 1U;

    ns = vfsd_namespace_find(client->ns_id);
    if (!ns)
        return -ENOENT;

    return vfsd_namespace_mount_add(ns, target_virtual, backend, readonly, 0U);
}

static int vfsd_client_umount(vfsd_client_t *client, const char *path)
{
    vfsd_namespace_t *ns;
    char              target_virtual[VFSD_IO_BYTES];
    int               rc;

    if (!client || !path)
        return -EINVAL;

    rc = vfsd_path_normalize(client->cwd, path, target_virtual, sizeof(target_virtual));
    if (rc < 0)
        return rc;

    ns = vfsd_namespace_find(client->ns_id);
    if (!ns)
        return -ENOENT;

    return vfsd_namespace_umount(ns, target_virtual);
}

static int vfsd_client_unshare(vfsd_client_t *client, u32 flags)
{
    vfsd_namespace_t *clone;

    if (!client)
        return -EINVAL;
    if (flags != CLONE_NEWNS)
        return -EINVAL;

    clone = vfsd_namespace_clone(client->ns_id);
    if (!clone)
        return -ENOMEM;

    client->ns_id = clone->ns_id;
    return 0;
}

static int vfsd_client_pivot_root(vfsd_client_t *client,
                                  const char *new_root,
                                  const char *old_root)
{
    vfsd_namespace_t *ns;
    vfsd_ns_mount_t  *root_mount;
    char              new_virtual[VFSD_IO_BYTES];
    char              old_virtual[VFSD_IO_BYTES];
    char              old_rebased[VFSD_IO_BYTES];
    char              new_backend[VFSD_PATH_BYTES];
    char              old_backend[VFSD_PATH_BYTES];
    stat_t            st;
    int               rc;

    if (!client || !new_root || !old_root)
        return -EINVAL;

    rc = vfsd_path_normalize(client->cwd, new_root, new_virtual, sizeof(new_virtual));
    if (rc < 0)
        return rc;
    rc = vfsd_path_normalize(client->cwd, old_root, old_virtual, sizeof(old_virtual));
    if (rc < 0)
        return rc;

    if (vfsd_streq(new_virtual, "/") || vfsd_streq(old_virtual, "/"))
        return -EINVAL;
    if (vfsd_streq(new_virtual, old_virtual))
        return -EINVAL;
    if (!vfsd_path_has_prefix(old_virtual, new_virtual))
        return -EINVAL;
    rc = vfsd_path_rebase_under(new_virtual, old_virtual, old_rebased, sizeof(old_rebased));
    if (rc < 0)
        return rc;
    if (vfsd_streq(old_rebased, "/"))
        return -EINVAL;

    rc = vfsd_client_backend_path(client, new_root, new_virtual, new_backend);
    if (rc < 0)
        return rc;
    rc = vfsd_boot_path_stat(new_backend, &st);
    if (rc < 0)
        return rc;
    if ((st.st_mode & S_IFMT) != S_IFDIR)
        return -ENOTDIR;

    ns = vfsd_namespace_find(client->ns_id);
    if (!ns)
        return -ENOENT;
    root_mount = vfsd_namespace_mount_exact(ns, "/");
    if (!root_mount)
        return -EIO;
    if (vfsd_namespace_mount_exact(ns, old_rebased))
        return -EBUSY;

    vfsd_strlcpy(old_backend, root_mount->backend, sizeof(old_backend));
    rc = vfsd_namespace_mount_add(ns, old_rebased, old_backend,
                                  root_mount->readonly,
                                  root_mount->linux_compat);
    if (rc < 0)
        return rc;

    vfsd_strlcpy(root_mount->backend, new_backend, sizeof(root_mount->backend));
    root_mount->readonly = vfsd_backend_readonly(new_backend);
    vfsd_strlcpy(client->cwd, "/", sizeof(client->cwd));
    return 0;
}

static int vfsd_client_respond_path(u32 port_id, const char *path)
{
    vfsd_response_t resp;
    u32             len;

    if (!path)
        return -EINVAL;

    len = vfsd_strlen(path) + 1U;
    if (len > VFSD_IO_BYTES)
        return -ENAMETOOLONG;

    vfsd_bzero(&resp, sizeof(resp));
    resp.status = 0;
    resp.data_len = len;
    vfsd_memcpy(resp.u.data, path, len);
    return (int)sys_ipc_reply_msg(port_id, IPC_MSG_VFS_RESP, &resp, (u32)sizeof(resp));
}

void _start(void)
{
    static const char port_name[] = "vfs";
    ipc_message_t     msg;
    u32               port_id;
    long              rc;

    vfsd_namespace_init();
    vfsd_bzero(g_clients, sizeof(g_clients));

    rc = sys_port_lookup_name(port_name);
    if (rc < 0) {
        vfsd_puts("[VFSD] port lookup fallita\n");
        sys_exit_now(1);
    }

    port_id = (u32)rc;
    vfsd_puts("[VFSD] online\n");

    for (;;) {
        const vfsd_request_t *req;
        vfsd_response_t       resp;
        vfsd_client_t        *client = NULL;

        rc = sys_ipc_wait_msg(port_id, &msg);
        if (rc < 0)
            continue;
        if (msg.msg_type != IPC_MSG_VFS_REQ)
            continue;
        if (msg.msg_len < (u32)sizeof(vfsd_request_t)) {
            vfsd_reply_error(port_id, -(long)EINVAL);
            continue;
        }

        req = (const vfsd_request_t *)msg.payload;
        vfsd_bzero(&resp, sizeof(resp));

        if (req->op == VFSD_REQ_OPEN || req->op == VFSD_REQ_RESOLVE ||
            req->op == VFSD_REQ_CHDIR || req->op == VFSD_REQ_GETCWD ||
            req->op == VFSD_REQ_MOUNT || req->op == VFSD_REQ_UMOUNT ||
            req->op == VFSD_REQ_UNSHARE || req->op == VFSD_REQ_PIVOT_ROOT) {
            client = vfsd_client_get_or_create(msg.sender_tid);
            if (!client) {
                vfsd_reply_error(port_id, -(long)ENOMEM);
                continue;
            }
        }

        switch (req->op) {
        case VFSD_REQ_OPEN: {
            char backend[VFSD_PATH_BYTES];
            char virtual_path[VFSD_IO_BYTES];
            vfsd_namespace_t *ns;

            rc = vfsd_client_virtual_path(client, req->u.paths.path, virtual_path);
            if (rc < 0) {
                resp.status = (int)rc;
                break;
            }
            ns = vfsd_namespace_find(client->ns_id);
            if (!ns) {
                resp.status = -ENOENT;
                break;
            }
            if ((req->flags & O_CREAT) != 0U) {
                rc = vfsd_namespace_resolve(ns, virtual_path, backend, NULL, NULL);
                if (rc < 0) {
                    resp.status = (int)rc;
                    break;
                }
                rc = sys_vfs_boot_open_now(backend, req->flags);
                if (rc < 0)
                    resp.status = (int)rc;
                else {
                    resp.status = 0;
                    resp.handle = (int)rc;
                }
            } else {
                rc = vfsd_namespace_open_existing(ns, virtual_path, req->flags, backend);
                if (rc < 0)
                    resp.status = (int)rc;
                else {
                    resp.status = 0;
                    resp.handle = (int)rc;
                }
            }
            break;
        }
        case VFSD_REQ_READ:
            rc = sys_vfs_boot_read_now((u32)req->handle, resp.u.data,
                                       (req->count > VFSD_IO_BYTES) ? VFSD_IO_BYTES : req->count);
            if (rc < 0)
                resp.status = (int)rc;
            else {
                resp.status = 0;
                resp.data_len = (u32)rc;
            }
            break;
        case VFSD_REQ_WRITE:
            rc = sys_vfs_boot_write_now((u32)req->handle, req->u.data,
                                        (req->count > VFSD_IO_BYTES) ? VFSD_IO_BYTES : req->count);
            if (rc < 0)
                resp.status = (int)rc;
            else {
                resp.status = 0;
                resp.data_len = (u32)rc;
            }
            break;
        case VFSD_REQ_READDIR:
            rc = sys_vfs_boot_readdir_now((u32)req->handle, &resp.u.dirent);
            resp.status = (int)rc;
            break;
        case VFSD_REQ_STAT:
            rc = sys_vfs_boot_stat_now((u32)req->handle, &resp.u.st);
            resp.status = (int)rc;
            break;
        case VFSD_REQ_CLOSE:
            rc = sys_vfs_boot_close_now((u32)req->handle);
            resp.status = (int)rc;
            break;
        case VFSD_REQ_LSEEK: {
            s64 offset = 0;

            vfsd_memcpy(&offset, req->u.data, sizeof(offset));
            rc = sys_vfs_boot_lseek_now((u32)req->handle, offset, req->arg0);
            if (rc < 0) {
                resp.status = (int)rc;
            } else {
                s64 new_pos = (s64)rc;

                resp.status = 0;
                resp.data_len = (u32)sizeof(new_pos);
                vfsd_memcpy(resp.u.data, &new_pos, sizeof(new_pos));
            }
            break;
        }
        case VFSD_REQ_RESOLVE: {
            char backend[VFSD_PATH_BYTES];
            char virtual_path[VFSD_IO_BYTES];
            u8   linux_compat = 0U;

            if (vfsd_client_virtual_path(client, req->u.paths.path, virtual_path) < 0) {
                resp.status = -ENAMETOOLONG;
                break;
            }
            {
                vfsd_namespace_t *ns = vfsd_namespace_find(client->ns_id);
                if (!ns) {
                    resp.status = -ENOENT;
                    break;
                }
                rc = vfsd_namespace_resolve_existing(ns, virtual_path, backend, NULL,
                                                     &linux_compat);
            }
            if (rc < 0) {
                resp.status = (int)rc;
                break;
            }
            resp.status = 0;
            resp.handle = linux_compat ? (int)VFSD_RESP_FLAG_LINUX_COMPAT : 0;
            resp.data_len = vfsd_strlen(backend) + 1U;
            if (resp.data_len > VFSD_IO_BYTES) {
                resp.status = -ENAMETOOLONG;
                break;
            }
            vfsd_memcpy(resp.u.data, backend, resp.data_len);
            (void)sys_ipc_reply_msg(port_id, IPC_MSG_VFS_RESP, &resp, (u32)sizeof(resp));
            continue;
        }
        case VFSD_REQ_CHDIR:
            resp.status = vfsd_client_chdir(client, req->u.paths.path);
            break;
        case VFSD_REQ_GETCWD:
            (void)req;
            (void)vfsd_client_respond_path(port_id, client->cwd);
            continue;
        case VFSD_REQ_MOUNT:
            resp.status = vfsd_client_mount(client, req->arg0, req->flags,
                                            req->u.paths.path, req->u.paths.aux);
            break;
        case VFSD_REQ_UMOUNT:
            resp.status = vfsd_client_umount(client, req->u.paths.path);
            break;
        case VFSD_REQ_UNSHARE:
            resp.status = vfsd_client_unshare(client, req->flags);
            break;
        case VFSD_REQ_PIVOT_ROOT:
            resp.status = vfsd_client_pivot_root(client,
                                                 req->u.paths.path,
                                                 req->u.paths.aux);
            break;
        default:
            resp.status = -(int)ENOSYS;
            break;
        }

        (void)sys_ipc_reply_msg(port_id, IPC_MSG_VFS_RESP, &resp, (u32)sizeof(resp));
    }
}
