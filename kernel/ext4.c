/*
 * EnlilOS Microkernel - ext4 read-only mount & read path (M5-03)
 *
 * Scope iniziale:
 *   - mount read-only di un'immagine ext4 su virtio-blk
 *   - parsing superblock, group descriptors, inode table
 *   - extent tree + fallback legacy (direct + single indirect)
 *   - open, read, readdir, stat, close via VFS
 *
 * Limiti deliberati di questa milestone:
 *   - nessun write path
 *   - nessun journal replay
 *   - nessun supporto inline-data / journal device / meta_bg
 *   - mount singleton su blk0, condiviso da /data e /sysroot
 */

#include "ext4.h"
#include "blk.h"
#include "uart.h"

#define EXT4_SUPER_OFFSET                1024ULL
#define EXT4_SUPER_SIZE                  1024U
#define EXT4_SUPER_SECTOR                (EXT4_SUPER_OFFSET / BLK_SECTOR_SIZE)
#define EXT4_SUPER_SECTORS               (EXT4_SUPER_SIZE / BLK_SECTOR_SIZE)
#define EXT4_SUPER_MAGIC                 0xEF53U
#define EXT4_ROOT_INO                    2U
#define EXT4_MAX_BLOCK_SIZE              4096U
#define EXT4_BLOCK_CACHE_SLOTS           8U
#define EXT4_INODE_CACHE_SLOTS           16U
#define EXT4_EXT_MAGIC                   0xF30AU

#define EXT4_FT_UNKNOWN                  0U
#define EXT4_FT_REG_FILE                 1U
#define EXT4_FT_DIR                      2U
#define EXT4_FT_CHRDEV                   3U
#define EXT4_FT_BLKDEV                   4U
#define EXT4_FT_FIFO                     5U
#define EXT4_FT_SOCK                     6U
#define EXT4_FT_SYMLINK                  7U

#define EXT4_FEATURE_INCOMPAT_FILETYPE   0x0002U
#define EXT4_FEATURE_INCOMPAT_RECOVER    0x0004U
#define EXT4_FEATURE_INCOMPAT_JOURNALDEV 0x0008U
#define EXT4_FEATURE_INCOMPAT_META_BG    0x0010U

#define EXT4_INODE_FLAG_EXTENTS          0x00080000U
#define EXT4_INODE_FLAG_INLINE_DATA      0x10000000U

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    uint8_t  s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;
} ext4_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} ext4_group_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint8_t  i_block[60];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
} ext4_inode_t;

typedef struct __attribute__((packed)) {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ext4_extent_header_t;

typedef struct __attribute__((packed)) {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_extent_t;

typedef struct __attribute__((packed)) {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_extent_idx_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} ext4_dir_entry_2_t;

typedef struct {
    bool     valid;
    uint64_t block_no;
    uint32_t stamp;
    uint8_t  data[EXT4_MAX_BLOCK_SIZE];
} ext4_block_cache_t;

typedef struct {
    bool         valid;
    uint32_t     ino;
    uint32_t     stamp;
    ext4_inode_t inode;
} ext4_inode_cache_t;

typedef struct {
    bool               mounted;
    bool               has_filetype;
    uint32_t           block_size;
    uint32_t           sectors_per_block;
    uint32_t           inode_size;
    uint32_t           blocks_per_group;
    uint32_t           inodes_per_group;
    uint32_t           group_count;
    uint32_t           desc_size;
    uint32_t           first_data_block;
    uint32_t           feature_compat;
    uint32_t           feature_incompat;
    uint32_t           feature_ro_compat;
    uint64_t           block_count;
    uint32_t           clock;
    char               label[17];
    char               status[160];
    ext4_superblock_t  sb;
    ext4_block_cache_t blocks[EXT4_BLOCK_CACHE_SLOTS];
    ext4_inode_cache_t inodes[EXT4_INODE_CACHE_SLOTS];
} ext4_state_t;

static ext4_state_t g_ext4;

static void e_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void e_memset(void *dst, uint8_t v, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

static size_t e_strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int e_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == *b);
}

static int e_nameeq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static void e_strlcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    if (max == 0) return;
    while (src && src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void status_reset(void)
{
    g_ext4.status[0] = '\0';
}

static void status_append(const char *s)
{
    size_t len = e_strlen(g_ext4.status);
    size_t i = 0;
    size_t cap = sizeof(g_ext4.status);

    while (s[i] && len + 1 < cap) {
        g_ext4.status[len++] = s[i++];
    }
    g_ext4.status[len] = '\0';
}

static void status_append_u32(uint32_t v)
{
    char tmp[10];
    size_t len = 0;

    if (v == 0U) {
        status_append("0");
        return;
    }

    while (v && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (v % 10U));
        v /= 10U;
    }

    while (len > 0U) {
        char one[2];
        one[0] = tmp[--len];
        one[1] = '\0';
        status_append(one);
    }
}

static void status_append_u64(uint64_t v)
{
    char tmp[20];
    size_t len = 0;

    if (v == 0ULL) {
        status_append("0");
        return;
    }

    while (v && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (v % 10ULL));
        v /= 10ULL;
    }

    while (len > 0U) {
        char one[2];
        one[0] = tmp[--len];
        one[1] = '\0';
        status_append(one);
    }
}

static void ext4_reset_state(void)
{
    e_memset(&g_ext4, 0, sizeof(g_ext4));
    e_strlcpy(g_ext4.label, "blk0", sizeof(g_ext4.label));
    e_strlcpy(g_ext4.status, "ext4 non inizializzato", sizeof(g_ext4.status));
}

static uint64_t ext4_inode_size(const ext4_inode_t *inode)
{
    return (uint64_t)inode->i_size_lo |
           ((uint64_t)inode->i_size_high << 32);
}

static uint64_t ext4_inode_blocks(const ext4_inode_t *inode)
{
    return (uint64_t)inode->i_blocks_lo |
           ((uint64_t)inode->i_blocks_high << 32);
}

static int ext4_inode_is_dir(const ext4_inode_t *inode)
{
    return (inode->i_mode & S_IFMT) == S_IFDIR;
}

static int ext4_inode_has_extents(const ext4_inode_t *inode)
{
    return (inode->i_flags & EXT4_INODE_FLAG_EXTENTS) != 0U;
}

static void ext4_log_status(const char *prefix)
{
    uart_puts(prefix);
    uart_puts(g_ext4.status);
    uart_puts("\n");
}

static void ext4_set_error(const char *msg)
{
    status_reset();
    status_append(msg);
    g_ext4.mounted = false;
    ext4_log_status("[EXT4] ");
}

static void ext4_cache_invalidate(void)
{
    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++)
        g_ext4.blocks[i].valid = false;
    for (uint32_t i = 0U; i < EXT4_INODE_CACHE_SLOTS; i++)
        g_ext4.inodes[i].valid = false;
    g_ext4.clock = 1U;
}

static int ext4_cache_get(uint64_t block_no, const uint8_t **out)
{
    ext4_block_cache_t *victim = NULL;
    uint32_t oldest = 0xFFFFFFFFU;

    if (!g_ext4.mounted) return -EIO;
    if (!out) return -EFAULT;

    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        if (g_ext4.blocks[i].valid && g_ext4.blocks[i].block_no == block_no) {
            g_ext4.blocks[i].stamp = ++g_ext4.clock;
            *out = g_ext4.blocks[i].data;
            return 0;
        }
    }

    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        if (!g_ext4.blocks[i].valid) {
            victim = &g_ext4.blocks[i];
            break;
        }
        if (g_ext4.blocks[i].stamp < oldest) {
            oldest = g_ext4.blocks[i].stamp;
            victim = &g_ext4.blocks[i];
        }
    }

    if (!victim) return -EIO;

    if (blk_read_sync(block_no * g_ext4.sectors_per_block,
                      victim->data, g_ext4.sectors_per_block) != BLK_OK)
        return -EIO;

    victim->valid = true;
    victim->block_no = block_no;
    victim->stamp = ++g_ext4.clock;
    *out = victim->data;
    return 0;
}

static int ext4_read_bytes(uint64_t offset, void *buf, size_t count)
{
    uint8_t *dst = (uint8_t *)buf;

    if (!g_ext4.mounted) return -EIO;
    if (!buf && count != 0U) return -EFAULT;

    while (count > 0U) {
        const uint8_t *block;
        uint64_t       block_no = offset / g_ext4.block_size;
        uint32_t       block_off = (uint32_t)(offset % g_ext4.block_size);
        size_t         chunk = g_ext4.block_size - block_off;
        int            rc;

        if (chunk > count) chunk = count;
        rc = ext4_cache_get(block_no, &block);
        if (rc < 0) return rc;

        e_memcpy(dst, block + block_off, chunk);
        dst += chunk;
        offset += chunk;
        count -= chunk;
    }

    return 0;
}

static int ext4_read_group_desc(uint32_t group, ext4_group_desc_t *out)
{
    uint8_t  raw[64];
    uint64_t gdt_block;
    uint64_t offset;

    if (!out) return -EFAULT;
    if (group >= g_ext4.group_count) return -EINVAL;
    if (g_ext4.desc_size > sizeof(raw)) return -EIO;

    gdt_block = (g_ext4.block_size == 1024U) ? 2ULL : 1ULL;
    offset = gdt_block * g_ext4.block_size +
             (uint64_t)group * g_ext4.desc_size;

    e_memset(raw, 0, sizeof(raw));
    if (ext4_read_bytes(offset, raw, g_ext4.desc_size) < 0)
        return -EIO;

    e_memset(out, 0, sizeof(*out));
    e_memcpy(out, raw,
             (g_ext4.desc_size < sizeof(*out)) ? g_ext4.desc_size : sizeof(*out));
    return 0;
}

static int ext4_load_inode_uncached(uint32_t ino, ext4_inode_t *out)
{
    ext4_group_desc_t gd;
    uint64_t          table_block;
    uint64_t          offset;
    uint32_t          group;
    uint32_t          index;

    if (!out) return -EFAULT;
    if (ino == 0U) return -EINVAL;

    group = (ino - 1U) / g_ext4.inodes_per_group;
    index = (ino - 1U) % g_ext4.inodes_per_group;

    if (ext4_read_group_desc(group, &gd) < 0)
        return -EIO;

    table_block = (uint64_t)gd.bg_inode_table_lo |
                  ((uint64_t)gd.bg_inode_table_hi << 32);
    offset = table_block * g_ext4.block_size +
             (uint64_t)index * g_ext4.inode_size;

    e_memset(out, 0, sizeof(*out));
    if (ext4_read_bytes(offset, out,
                        (g_ext4.inode_size < sizeof(*out))
                            ? g_ext4.inode_size
                            : sizeof(*out)) < 0)
        return -EIO;

    return 0;
}

static int ext4_load_inode(uint32_t ino, ext4_inode_t *out)
{
    ext4_inode_cache_t *victim = NULL;
    uint32_t            oldest = 0xFFFFFFFFU;

    if (!g_ext4.mounted) return -EIO;
    if (!out) return -EFAULT;

    for (uint32_t i = 0U; i < EXT4_INODE_CACHE_SLOTS; i++) {
        if (g_ext4.inodes[i].valid && g_ext4.inodes[i].ino == ino) {
            g_ext4.inodes[i].stamp = ++g_ext4.clock;
            *out = g_ext4.inodes[i].inode;
            return 0;
        }
    }

    for (uint32_t i = 0U; i < EXT4_INODE_CACHE_SLOTS; i++) {
        if (!g_ext4.inodes[i].valid) {
            victim = &g_ext4.inodes[i];
            break;
        }
        if (g_ext4.inodes[i].stamp < oldest) {
            oldest = g_ext4.inodes[i].stamp;
            victim = &g_ext4.inodes[i];
        }
    }

    if (!victim) return -EIO;
    if (ext4_load_inode_uncached(ino, &victim->inode) < 0)
        return -EIO;

    victim->valid = true;
    victim->ino = ino;
    victim->stamp = ++g_ext4.clock;
    *out = victim->inode;
    return 0;
}

static int ext4_extent_map(const ext4_inode_t *inode, uint32_t lblock,
                           uint64_t *pblock)
{
    const uint8_t            *node = inode->i_block;
    const ext4_extent_header_t *hdr = (const ext4_extent_header_t *)node;

    if (!pblock) return -EFAULT;
    *pblock = 0ULL;

    if (hdr->eh_magic != EXT4_EXT_MAGIC)
        return -EIO;

    while (1) {
        if (hdr->eh_entries > hdr->eh_max)
            return -EIO;

        if (hdr->eh_depth == 0U) {
            const ext4_extent_t *ext =
                (const ext4_extent_t *)(node + sizeof(*hdr));

            for (uint16_t i = 0U; i < hdr->eh_entries; i++) {
                uint32_t start = ext[i].ee_block;
                uint32_t len = ext[i].ee_len;
                uint64_t phys;
                int      uninit = 0;

                if (len > 32768U) {
                    len -= 32768U;
                    uninit = 1;
                }
                if (len == 0U) len = 32768U;

                if (lblock < start || lblock >= start + len)
                    continue;

                if (uninit) {
                    *pblock = 0ULL;
                    return 0;
                }

                phys = ((uint64_t)ext[i].ee_start_hi << 32) |
                       (uint64_t)ext[i].ee_start_lo;
                *pblock = phys + (uint64_t)(lblock - start);
                return 0;
            }

            return 0;
        } else {
            const ext4_extent_idx_t *idx =
                (const ext4_extent_idx_t *)(node + sizeof(*hdr));
            uint16_t chosen = 0U;
            const uint8_t *next;
            uint64_t child;
            int rc;

            for (uint16_t i = 0U; i < hdr->eh_entries; i++) {
                if (idx[i].ei_block > lblock)
                    break;
                chosen = i;
            }

            child = ((uint64_t)idx[chosen].ei_leaf_hi << 32) |
                    (uint64_t)idx[chosen].ei_leaf_lo;
            rc = ext4_cache_get(child, &next);
            if (rc < 0) return rc;

            node = next;
            hdr = (const ext4_extent_header_t *)node;
            if (hdr->eh_magic != EXT4_EXT_MAGIC)
                return -EIO;
        }
    }
}

static int ext4_legacy_map(const ext4_inode_t *inode, uint32_t lblock,
                           uint64_t *pblock)
{
    const uint32_t *ptrs = (const uint32_t *)(const void *)inode->i_block;

    if (!pblock) return -EFAULT;
    *pblock = 0ULL;

    if (lblock < 12U) {
        *pblock = ptrs[lblock];
        return 0;
    }

    lblock -= 12U;
    if (lblock < g_ext4.block_size / sizeof(uint32_t) && ptrs[12] != 0U) {
        const uint8_t  *blk;
        const uint32_t *indirect;
        int             rc = ext4_cache_get(ptrs[12], &blk);
        if (rc < 0) return rc;
        indirect = (const uint32_t *)(const void *)blk;
        *pblock = indirect[lblock];
        return 0;
    }

    return 0;
}

static int ext4_map_lblock(const ext4_inode_t *inode, uint32_t lblock,
                           uint64_t *pblock)
{
    if (!inode) return -EFAULT;
    if (inode->i_flags & EXT4_INODE_FLAG_INLINE_DATA)
        return -EIO;

    if (ext4_inode_has_extents(inode))
        return ext4_extent_map(inode, lblock, pblock);

    return ext4_legacy_map(inode, lblock, pblock);
}

static int ext4_read_inode_data(const ext4_inode_t *inode, uint64_t pos,
                                void *buf, size_t count)
{
    uint8_t *dst = (uint8_t *)buf;
    uint64_t size;

    if (!inode) return -EFAULT;
    if (!buf && count != 0U) return -EFAULT;

    size = ext4_inode_size(inode);
    if (pos >= size) return 0;
    if (count > (size_t)(size - pos))
        count = (size_t)(size - pos);

    while (count > 0U) {
        uint32_t       lblock = (uint32_t)(pos / g_ext4.block_size);
        uint32_t       block_off = (uint32_t)(pos % g_ext4.block_size);
        size_t         chunk = g_ext4.block_size - block_off;
        uint64_t       pblock = 0ULL;
        const uint8_t *src = NULL;
        int            rc;

        if (chunk > count) chunk = count;

        rc = ext4_map_lblock(inode, lblock, &pblock);
        if (rc < 0) return rc;

        if (pblock != 0ULL) {
            rc = ext4_cache_get(pblock, &src);
            if (rc < 0) return rc;
            e_memcpy(dst, src + block_off, chunk);
        } else {
            e_memset(dst, 0U, chunk);
        }

        dst += chunk;
        pos += chunk;
        count -= chunk;
    }

    return 0;
}

static uint32_t ext4_mode_from_ftype(uint8_t ft)
{
    switch (ft) {
    case EXT4_FT_REG_FILE: return S_IFREG;
    case EXT4_FT_DIR:      return S_IFDIR;
    case EXT4_FT_CHRDEV:   return S_IFCHR;
    default:               return 0U;
    }
}

static int ext4_lookup_child(uint32_t dir_ino, const ext4_inode_t *dir_inode,
                             const char *name, size_t name_len,
                             uint32_t *child_ino, ext4_inode_t *child_inode)
{
    uint64_t dir_size;
    uint64_t pos = 0ULL;

    if (!dir_inode || !child_ino || !child_inode) return -EFAULT;
    if (!ext4_inode_is_dir(dir_inode)) return -ENOTDIR;

    dir_size = ext4_inode_size(dir_inode);
    while (pos < dir_size) {
        uint32_t       lblock = (uint32_t)(pos / g_ext4.block_size);
        uint32_t       block_off = (uint32_t)(pos % g_ext4.block_size);
        uint64_t       pblock = 0ULL;
        const uint8_t *block;
        int            rc = ext4_map_lblock(dir_inode, lblock, &pblock);

        if (rc < 0) return rc;
        if (pblock == 0ULL) {
            pos = ((uint64_t)lblock + 1ULL) * g_ext4.block_size;
            continue;
        }

        rc = ext4_cache_get(pblock, &block);
        if (rc < 0) return rc;

        while (block_off + 8U <= g_ext4.block_size && pos < dir_size) {
            const ext4_dir_entry_2_t *de =
                (const ext4_dir_entry_2_t *)(const void *)(block + block_off);
            uint16_t rec_len = de->rec_len;
            uint16_t dname_len;

            if (rec_len < 8U || (rec_len & 3U) != 0U ||
                block_off + rec_len > g_ext4.block_size)
                return -EIO;

            dname_len = g_ext4.has_filetype ? de->name_len
                                            : *(const uint16_t *)&block[block_off + 6U];

            if (de->inode != 0U &&
                dname_len <= rec_len - 8U &&
                e_nameeq(de->name, dname_len, name, name_len)) {
                *child_ino = de->inode;
                return ext4_load_inode(*child_ino, child_inode);
            }

            block_off += rec_len;
            pos += rec_len;
        }
    }

    (void)dir_ino;
    return -ENOENT;
}

static int ext4_lookup_path(const char *relpath, uint32_t *ino, ext4_inode_t *inode)
{
    const char *p = relpath;
    uint32_t    cur_ino = EXT4_ROOT_INO;
    ext4_inode_t cur_inode;
    int         rc;

    if (!ino || !inode) return -EFAULT;
    if (!g_ext4.mounted) return -EIO;

    rc = ext4_load_inode(cur_ino, &cur_inode);
    if (rc < 0) return rc;

    if (!p || p[0] == '\0' || e_streq(p, "/")) {
        *ino = cur_ino;
        *inode = cur_inode;
        return 0;
    }

    while (*p == '/') p++;
    while (*p) {
        const char *start = p;
        size_t      len = 0U;

        while (*p && *p != '/') {
            p++;
            len++;
        }

        rc = ext4_lookup_child(cur_ino, &cur_inode, start, len, &cur_ino, &cur_inode);
        if (rc < 0) return rc;

        while (*p == '/') p++;
    }

    *ino = cur_ino;
    *inode = cur_inode;
    return 0;
}

static int ext4_open_vfs(const vfs_mount_t *mount, const char *relpath,
                         uint32_t flags, vfs_file_t *out)
{
    ext4_inode_t inode;
    uint32_t     ino;
    uint32_t     access = (flags & 0x3U);
    int          rc;

    if (!out) return -EFAULT;
    if (!g_ext4.mounted) return -EIO;

    if (access == O_WRONLY || access == O_RDWR ||
        (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0U)
        return -EROFS;

    rc = ext4_lookup_path(relpath, &ino, &inode);
    if (rc < 0) return rc;

    out->mount     = mount;
    out->node_id   = ino;
    out->flags     = flags;
    out->pos       = 0ULL;
    out->size_hint = ext4_inode_size(&inode);
    out->dir_index = 0U;
    out->cookie    = (uintptr_t)inode.i_mode;
    return 0;
}

static ssize_t ext4_read_vfs(vfs_file_t *file, void *buf, size_t count)
{
    ext4_inode_t inode;
    uint64_t     size;
    int          rc;

    if (!file) return -EBADF;
    if (!buf) return -EFAULT;

    rc = ext4_load_inode(file->node_id, &inode);
    if (rc < 0) return rc;
    if (ext4_inode_is_dir(&inode)) return -EISDIR;

    size = ext4_inode_size(&inode);
    if (file->pos >= size) return 0;
    if (count > (size_t)(size - file->pos))
        count = (size_t)(size - file->pos);

    rc = ext4_read_inode_data(&inode, file->pos, buf, count);
    if (rc < 0) return rc;

    file->pos += count;
    return (ssize_t)count;
}

static ssize_t ext4_write_vfs(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    return -EROFS;
}

static int ext4_readdir_vfs(vfs_file_t *file, vfs_dirent_t *out)
{
    ext4_inode_t dir_inode;
    uint64_t     dir_size;
    int          rc;

    if (!file || !out) return -EFAULT;

    rc = ext4_load_inode(file->node_id, &dir_inode);
    if (rc < 0) return rc;
    if (!ext4_inode_is_dir(&dir_inode)) return -ENOTDIR;

    dir_size = ext4_inode_size(&dir_inode);
    while (file->pos < dir_size) {
        uint32_t       lblock = (uint32_t)(file->pos / g_ext4.block_size);
        uint32_t       block_off = (uint32_t)(file->pos % g_ext4.block_size);
        uint64_t       pblock = 0ULL;
        const uint8_t *block;
        const ext4_dir_entry_2_t *de;
        uint16_t       rec_len;
        uint16_t       name_len;
        uint32_t       mode;

        rc = ext4_map_lblock(&dir_inode, lblock, &pblock);
        if (rc < 0) return rc;
        if (pblock == 0ULL) {
            file->pos = ((uint64_t)lblock + 1ULL) * g_ext4.block_size;
            continue;
        }

        rc = ext4_cache_get(pblock, &block);
        if (rc < 0) return rc;

        de = (const ext4_dir_entry_2_t *)(const void *)(block + block_off);
        rec_len = de->rec_len;
        if (rec_len < 8U || (rec_len & 3U) != 0U ||
            block_off + rec_len > g_ext4.block_size)
            return -EIO;

        file->pos += rec_len;

        name_len = g_ext4.has_filetype ? de->name_len
                                       : *(const uint16_t *)&block[block_off + 6U];
        if (de->inode == 0U || name_len == 0U || name_len > rec_len - 8U)
            continue;

        e_strlcpy(out->name, "", sizeof(out->name));
        for (uint16_t i = 0U; i < name_len && i + 1U < sizeof(out->name); i++)
            out->name[i] = de->name[i];
        out->name[(name_len < sizeof(out->name)) ? name_len : (sizeof(out->name) - 1U)] = '\0';

        mode = g_ext4.has_filetype ? ext4_mode_from_ftype(de->file_type) : 0U;
        if (mode == 0U) {
            ext4_inode_t child;
            rc = ext4_load_inode(de->inode, &child);
            if (rc < 0) return rc;
            mode = child.i_mode;
        }
        out->mode = mode;
        return 0;
    }

    return -ENOENT;
}

static int ext4_stat_vfs(vfs_file_t *file, stat_t *out)
{
    ext4_inode_t inode;
    int rc;

    if (!file || !out) return -EFAULT;
    rc = ext4_load_inode(file->node_id, &inode);
    if (rc < 0) return rc;

    out->st_mode = inode.i_mode;
    out->st_blksize = g_ext4.block_size;
    out->st_size = ext4_inode_size(&inode);
    out->st_blocks = ext4_inode_blocks(&inode);
    return 0;
}

static int ext4_close_vfs(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static const vfs_ops_t ext4_ops = {
    .open    = ext4_open_vfs,
    .read    = ext4_read_vfs,
    .write   = ext4_write_vfs,
    .readdir = ext4_readdir_vfs,
    .stat    = ext4_stat_vfs,
    .close   = ext4_close_vfs,
};

int ext4_mount(void)
{
    ext4_superblock_t sb;
    uint8_t           sb_raw[EXT4_SUPER_SIZE];
    ext4_inode_t      root;
    uint64_t          total_blocks;
    uint64_t          groups;
    int               rc;

    if (!blk_is_ready()) {
        ext4_reset_state();
        ext4_set_error("virtio-blk assente");
        return -EIO;
    }

    if (g_ext4.mounted)
        return 0;

    ext4_reset_state();
    rc = blk_read_sync(EXT4_SUPER_SECTOR, sb_raw, EXT4_SUPER_SECTORS);
    if (rc != BLK_OK) {
        ext4_set_error("lettura superblock fallita");
        return -EIO;
    }
    e_memset(&sb, 0, sizeof(sb));
    e_memcpy(&sb, sb_raw, sizeof(sb));
    if (sb.s_magic != EXT4_SUPER_MAGIC) {
        ext4_set_error("superblock ext4 non trovato");
        return -EINVAL;
    }

    g_ext4.sb = sb;
    g_ext4.block_size = 1024U << sb.s_log_block_size;
    g_ext4.inode_size = (sb.s_inode_size != 0U) ? sb.s_inode_size : 128U;
    g_ext4.blocks_per_group = sb.s_blocks_per_group;
    g_ext4.inodes_per_group = sb.s_inodes_per_group;
    g_ext4.first_data_block = sb.s_first_data_block;
    g_ext4.feature_compat = sb.s_feature_compat;
    g_ext4.feature_incompat = sb.s_feature_incompat;
    g_ext4.feature_ro_compat = sb.s_feature_ro_compat;
    g_ext4.desc_size = (sb.s_desc_size != 0U) ? sb.s_desc_size : 32U;
    g_ext4.has_filetype =
        (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0U;
    total_blocks = (uint64_t)sb.s_blocks_count_lo |
                   ((uint64_t)sb.s_blocks_count_hi << 32);
    g_ext4.block_count = total_blocks;

    if (g_ext4.block_size == 0U ||
        g_ext4.block_size > EXT4_MAX_BLOCK_SIZE ||
        (g_ext4.block_size & (g_ext4.block_size - 1U)) != 0U) {
        ext4_set_error("block size ext4 non supportata");
        return -EINVAL;
    }
    if (g_ext4.block_size < BLK_SECTOR_SIZE ||
        (g_ext4.block_size % BLK_SECTOR_SIZE) != 0U) {
        ext4_set_error("block size non allineata ai settori");
        return -EINVAL;
    }
    if (sb.s_log_cluster_size != sb.s_log_block_size) {
        ext4_set_error("bigalloc non supportato");
        return -EINVAL;
    }
    if (g_ext4.inode_size < 128U || g_ext4.inode_size > 512U) {
        ext4_set_error("inode size ext4 non supportata");
        return -EINVAL;
    }
    if (g_ext4.blocks_per_group == 0U || g_ext4.inodes_per_group == 0U) {
        ext4_set_error("superblock corrotto");
        return -EINVAL;
    }
    if (g_ext4.desc_size < 32U || g_ext4.desc_size > 64U) {
        ext4_set_error("descriptor size ext4 non supportata");
        return -EINVAL;
    }
    if (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
        ext4_set_error("journal replay richiesto: mount ro rifiutato");
        return -EROFS;
    }
    if (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_JOURNALDEV) {
        ext4_set_error("journal device esterno non supportato");
        return -EINVAL;
    }
    if (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) {
        ext4_set_error("meta_bg non supportato");
        return -EINVAL;
    }

    g_ext4.sectors_per_block = g_ext4.block_size / BLK_SECTOR_SIZE;
    groups = (total_blocks - g_ext4.first_data_block +
              g_ext4.blocks_per_group - 1ULL) / g_ext4.blocks_per_group;
    g_ext4.group_count = (uint32_t)groups;

    e_memset(g_ext4.label, 0, sizeof(g_ext4.label));
    for (uint32_t i = 0U; i < 16U && sb.s_volume_name[i] != '\0'; i++)
        g_ext4.label[i] = sb.s_volume_name[i];
    if (g_ext4.label[0] == '\0')
        e_strlcpy(g_ext4.label, "ext4", sizeof(g_ext4.label));

    g_ext4.mounted = true;
    ext4_cache_invalidate();

    rc = ext4_load_inode(EXT4_ROOT_INO, &root);
    if (rc < 0 || !ext4_inode_is_dir(&root)) {
        g_ext4.mounted = false;
        ext4_set_error("root inode ext4 non valida");
        return -EIO;
    }

    status_reset();
    status_append("mount ro OK: label=");
    status_append(g_ext4.label);
    status_append(" block=");
    status_append_u32(g_ext4.block_size);
    status_append(" groups=");
    status_append_u32(g_ext4.group_count);
    status_append(" blocks=");
    status_append_u64(g_ext4.block_count);
    ext4_log_status("[EXT4] ");
    return 0;
}

void ext4_unmount(void)
{
    ext4_reset_state();
}

int ext4_is_mounted(void)
{
    return g_ext4.mounted ? 1 : 0;
}

const char *ext4_status(void)
{
    return g_ext4.status;
}

const char *ext4_label(void)
{
    return g_ext4.label;
}

const vfs_ops_t *ext4_vfs_ops(void)
{
    return &ext4_ops;
}
