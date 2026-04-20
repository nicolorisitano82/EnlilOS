/*
 * EnlilOS Microkernel - ext4 mount, read path e write core (M5-03 / M5-04)
 *
 * Scope attuale:
 *   - mount di un'immagine ext4 su virtio-blk
 *   - parsing superblock, group descriptors, inode table
 *   - extent tree + fallback legacy (direct + single indirect)
 *   - open, read, write, readdir, stat, close via VFS
 *   - create, mkdir, unlink, rename e fsync lato VFS
 *   - allocazione bounded di blocchi/inode con metadata checksum
 *   - O_TRUNC / O_APPEND / O_CREAT e sync esplicito
 *
 * Limiti deliberati di questa milestone:
 *   - journal bounded custom di EnlilOS, non compatibile JBD2 Linux
 *   - nessun supporto inline-data / journal device / meta_bg
 *   - niente htree update: directory indicizzate restano read-only
 *   - extent tree modificabile solo a profondita' 0 (extent root in inode)
 *   - mount singleton su blk0, condiviso da /data e /sysroot
 */

#include "ext4.h"
#include "blk.h"
#include "kmon.h"
#include "sched.h"
#include "timer.h"
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
#define EXT4_DIR_TAIL_SIZE               12U
#define EXT4_NAME_MAX                    255U
#define EXT4_SUPER_CSUM_OFF              0x3FCU
#define EXT4_SUPER_CSUM_SEED_OFF         0x270U
#define EXT4_SUPER_STATE_CLEAN           0x0001U
#define EXT4_DEF_FILE_MODE               (S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
#define EXT4_DEF_DIR_MODE                (S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR | \
                                          S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)
#define EXT4_INTERNAL_JOURNAL_NAME       ".enlil-journal"
#define EXT4_JOURNAL_MAGIC               0x454A4E4CU
#define EXT4_JOURNAL_VERSION             1U
#define EXT4_JOURNAL_STATE_CLEAN         0U
#define EXT4_JOURNAL_STATE_PENDING       1U
#define EXT4_JOURNAL_HEADER_CRC_OFF      24U
#define EXT4_JOURNAL_MAX_TX_BLOCKS       (EXT4_BLOCK_CACHE_SLOTS + EXT4_INODE_CACHE_SLOTS)
#define EXT4_JOURNAL_FILE_BLOCKS         (1U + EXT4_JOURNAL_MAX_TX_BLOCKS)

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
#define EXT4_FEATURE_INCOMPAT_EXTENTS    0x0040U
#define EXT4_FEATURE_INCOMPAT_64BIT      0x0080U
#define EXT4_FEATURE_INCOMPAT_FLEX_BG    0x0200U
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED  0x2000U

#define EXT4_FEATURE_COMPAT_HAS_JOURNAL  0x0004U
#define EXT4_FEATURE_COMPAT_DIR_INDEX    0x0020U

#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400U

#define EXT4_INODE_FLAG_EXTENTS          0x00080000U
#define EXT4_INODE_FLAG_INDEX            0x00001000U
#define EXT4_INODE_FLAG_INLINE_DATA      0x10000000U

#define EXT4_ERRNO(e)                    (-(int)(e))

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

typedef struct __attribute__((packed)) {
    uint32_t reserved_zero1;
    uint16_t rec_len;
    uint8_t  reserved_zero2;
    uint8_t  reserved_ft;
    uint32_t checksum;
} ext4_dir_entry_tail_t;

typedef struct {
    bool     valid;
    bool     dirty;
    uint64_t block_no;
    uint32_t stamp;
    uint8_t  data[EXT4_MAX_BLOCK_SIZE];
} ext4_block_cache_t;

typedef struct {
    bool         valid;
    bool         dirty;
    uint32_t     ino;
    uint32_t     stamp;
    ext4_inode_t inode;
    uint8_t      raw[512];
} ext4_inode_cache_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t entry_count;
    uint32_t block_size;
    uint32_t sequence;
    uint32_t header_crc;
    uint32_t reserved;
} ext4_journal_header_t;

typedef struct __attribute__((packed)) {
    uint64_t target_block;
    uint32_t data_crc;
    uint32_t reserved;
} ext4_journal_entry_t;

typedef struct {
    uint32_t count;
    ext4_journal_entry_t entries[EXT4_JOURNAL_MAX_TX_BLOCKS];
    uint8_t              data[EXT4_JOURNAL_MAX_TX_BLOCKS][EXT4_MAX_BLOCK_SIZE];
} ext4_journal_txn_t;

typedef struct {
    bool     ready;
    bool     in_commit;
    uint32_t ino;
    uint32_t sequence;
    uint64_t phys_blocks[EXT4_JOURNAL_FILE_BLOCKS];
} ext4_journal_state_t;

typedef struct {
    bool               mounted;
    bool               has_filetype;
    bool               metadata_csum;
    bool               has_dir_index;
    bool               has_journal;
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
    uint32_t           checksum_seed;
    uint32_t           clock;
    uint64_t           last_dirty_ms;
    char               label[17];
    char               status[160];
    uint8_t            sb_raw[EXT4_SUPER_SIZE];
    ext4_superblock_t  sb;
    ext4_journal_state_t journal;
    ext4_block_cache_t blocks[EXT4_BLOCK_CACHE_SLOTS];
    ext4_inode_cache_t inodes[EXT4_INODE_CACHE_SLOTS];
} ext4_state_t;

static ext4_state_t g_ext4;
static ext4_journal_txn_t g_ext4_txn;
static kmon_t g_ext4_open_lock = KMON_INVALID;

static int ext4_open_lock_enter(void)
{
    if (g_ext4_open_lock == KMON_INVALID)
        return 0;
    return kmon_enter_current(g_ext4_open_lock);
}

static void ext4_open_lock_exit(int lock_rc)
{
    if (g_ext4_open_lock == KMON_INVALID || lock_rc < 0)
        return;
    (void)kmon_exit_current(g_ext4_open_lock);
}

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

static void e_memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0U) return;
    if (d < s) {
        while (n--) *d++ = *s++;
        return;
    }

    d += n;
    s += n;
    while (n--) *--d = *--s;
}

static inline uint16_t e_align4(uint16_t v)
{
    return (uint16_t)((v + 3U) & ~3U);
}

static inline uint16_t ext4_dir_rec_len(uint16_t name_len)
{
    return e_align4((uint16_t)(8U + name_len));
}

static uint32_t ext4_crc32c_raw(uint32_t crc, const void *buf, size_t len)
{
    static const uint32_t poly = 0x82F63B78U;
    const uint8_t *p = (const uint8_t *)buf;

    while (len-- > 0U) {
        crc ^= (uint32_t)(*p++);
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            if ((crc & 1U) != 0U)
                crc = (crc >> 1) ^ poly;
            else
                crc >>= 1;
        }
    }

    return crc;
}

static uint32_t ext4_seeded_checksum(const void *buf, size_t len)
{
    return ext4_crc32c_raw(g_ext4.checksum_seed, buf, len);
}

static inline uint64_t ext4_group_first_block(uint32_t group)
{
    return g_ext4.first_data_block + (uint64_t)group * g_ext4.blocks_per_group;
}

static uint32_t ext4_block_to_group(uint64_t block_no)
{
    if (block_no < g_ext4.first_data_block)
        return 0U;
    return (uint32_t)((block_no - g_ext4.first_data_block) / g_ext4.blocks_per_group);
}

static uint32_t ext4_block_group_bit(uint64_t block_no)
{
    return (uint32_t)(block_no - ext4_group_first_block(ext4_block_to_group(block_no)));
}

static uint32_t ext4_group_free_blocks(const ext4_group_desc_t *gd)
{
    return (uint32_t)gd->bg_free_blocks_count_lo |
           ((uint32_t)gd->bg_free_blocks_count_hi << 16);
}

static void ext4_group_set_free_blocks(ext4_group_desc_t *gd, uint32_t v)
{
    gd->bg_free_blocks_count_lo = (uint16_t)v;
    gd->bg_free_blocks_count_hi = (uint16_t)(v >> 16);
}

static uint32_t ext4_group_free_inodes(const ext4_group_desc_t *gd)
{
    return (uint32_t)gd->bg_free_inodes_count_lo |
           ((uint32_t)gd->bg_free_inodes_count_hi << 16);
}

static void ext4_group_set_free_inodes(ext4_group_desc_t *gd, uint32_t v)
{
    gd->bg_free_inodes_count_lo = (uint16_t)v;
    gd->bg_free_inodes_count_hi = (uint16_t)(v >> 16);
}

static uint32_t ext4_group_used_dirs(const ext4_group_desc_t *gd)
{
    return (uint32_t)gd->bg_used_dirs_count_lo |
           ((uint32_t)gd->bg_used_dirs_count_hi << 16);
}

static void ext4_group_set_used_dirs(ext4_group_desc_t *gd, uint32_t v)
{
    gd->bg_used_dirs_count_lo = (uint16_t)v;
    gd->bg_used_dirs_count_hi = (uint16_t)(v >> 16);
}

static void ext4_group_set_itable_unused(ext4_group_desc_t *gd, uint32_t v)
{
    gd->bg_itable_unused_lo = (uint16_t)v;
    gd->bg_itable_unused_hi = (uint16_t)(v >> 16);
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

static void ext4_mark_dirty_activity(void)
{
    g_ext4.last_dirty_ms = timer_now_ms();
}

static void ext4_inode_set_size(ext4_inode_t *inode, uint64_t size)
{
    inode->i_size_lo   = (uint32_t)size;
    inode->i_size_high = (uint32_t)(size >> 32);
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

static uint64_t ext4_inode_capacity_bytes(const ext4_inode_t *inode)
{
    if ((inode->i_flags & EXT4_INODE_FLAG_EXTENTS) != 0U)
        return g_ext4.block_count * (uint64_t)g_ext4.block_size;

    return (12ULL + (uint64_t)(g_ext4.block_size / sizeof(uint32_t))) *
           (uint64_t)g_ext4.block_size;
}

static int ext4_inode_is_dir(const ext4_inode_t *inode)
{
    return (inode->i_mode & S_IFMT) == S_IFDIR;
}

static int ext4_inode_is_symlink(const ext4_inode_t *inode)
{
    return (inode->i_mode & S_IFMT) == S_IFLNK;
}

static int ext4_inode_is_fast_symlink(const ext4_inode_t *inode)
{
    return ext4_inode_is_symlink(inode) && ext4_inode_blocks(inode) == 0ULL;
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

static int ext4_raw_block_read(uint64_t block_no, void *buf)
{
    if (blk_read_sync(block_no * g_ext4.sectors_per_block,
                      buf, g_ext4.sectors_per_block) != BLK_OK)
        return -EIO;
    return 0;
}

static int ext4_raw_block_write(uint64_t block_no, const void *buf)
{
    if (blk_write_sync(block_no * g_ext4.sectors_per_block,
                       buf, g_ext4.sectors_per_block) != BLK_OK)
        return -EIO;
    return 0;
}

static uint32_t ext4_journal_block_crc(const uint8_t *buf)
{
    return ext4_crc32c_raw(0xFFFFFFFFU, buf, g_ext4.block_size);
}

static int ext4_internal_journal_path(const char *relpath)
{
    const char *p = relpath;

    if (!p) return 0;
    while (*p == '/')
        p++;
    return e_streq(p, EXT4_INTERNAL_JOURNAL_NAME);
}

static int ext4_sync_raw(void);
static int ext4_commit_pending(void);

static int ext4_find_block_slot(uint64_t block_no)
{
    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        if (g_ext4.blocks[i].valid && g_ext4.blocks[i].block_no == block_no)
            return (int)i;
    }
    return -1;
}

static int ext4_cache_load(uint64_t block_no, ext4_block_cache_t **out)
{
    ext4_block_cache_t *victim = NULL;
    uint32_t            oldest = 0xFFFFFFFFU;
    int                 slot_idx;

    if (!g_ext4.mounted) return -EIO;
    if (!out) return -EFAULT;

    slot_idx = ext4_find_block_slot(block_no);
    if (slot_idx >= 0) {
        g_ext4.blocks[slot_idx].stamp = ++g_ext4.clock;
        *out = &g_ext4.blocks[slot_idx];
        return 0;
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
    if (victim->valid && victim->dirty) {
        if (ext4_commit_pending() < 0)
            return -EIO;
    }

    if (blk_read_sync(block_no * g_ext4.sectors_per_block,
                      victim->data, g_ext4.sectors_per_block) != BLK_OK)
        return -EIO;

    victim->valid = true;
    victim->dirty = false;
    victim->block_no = block_no;
    victim->stamp = ++g_ext4.clock;
    *out = victim;
    return 0;
}

static int ext4_cache_get(uint64_t block_no, const uint8_t **out)
{
    ext4_block_cache_t *slot;
    int rc;

    if (!out) return -EFAULT;
    rc = ext4_cache_load(block_no, &slot);
    if (rc < 0) return rc;
    *out = slot->data;
    return 0;
}

static int ext4_cache_get_mut(uint64_t block_no, uint8_t **out)
{
    ext4_block_cache_t *slot;
    int rc;

    if (!out) return -EFAULT;
    rc = ext4_cache_load(block_no, &slot);
    if (rc < 0) return rc;
    slot->dirty = true;
    ext4_mark_dirty_activity();
    *out = slot->data;
    return 0;
}

static int ext4_load_inode(uint32_t ino, ext4_inode_t *out);

static int ext4_map_lblock(const ext4_inode_t *inode, uint32_t lblock,
                           uint64_t *pblock);

static int ext4_mark_inode_dirty(uint32_t ino, const ext4_inode_t *inode)
{
    ext4_inode_cache_t *victim = NULL;
    uint32_t            oldest = 0xFFFFFFFFU;

    if (!inode) return -EFAULT;

    for (uint32_t i = 0U; i < EXT4_INODE_CACHE_SLOTS; i++) {
        if (g_ext4.inodes[i].valid && g_ext4.inodes[i].ino == ino) {
            g_ext4.inodes[i].inode = *inode;
            e_memcpy(g_ext4.inodes[i].raw, inode, sizeof(*inode));
            g_ext4.inodes[i].dirty = true;
            g_ext4.inodes[i].stamp = ++g_ext4.clock;
            ext4_mark_dirty_activity();
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
    if (victim->valid && victim->dirty) {
        if (ext4_commit_pending() < 0)
            return -EIO;
    }

    victim->valid = true;
    victim->dirty = true;
    victim->ino = ino;
    victim->inode = *inode;
    e_memset(victim->raw, 0U, sizeof(victim->raw));
    e_memcpy(victim->raw, inode, sizeof(*inode));
    victim->stamp = ++g_ext4.clock;
    ext4_mark_dirty_activity();
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

static int ext4_write_bytes_cached(uint64_t offset, const void *buf, size_t count)
{
    const uint8_t *src = (const uint8_t *)buf;

    if (!g_ext4.mounted) return -EIO;
    if (!buf && count != 0U) return -EFAULT;

    while (count > 0U) {
        uint8_t       *block;
        uint64_t       block_no = offset / g_ext4.block_size;
        uint32_t       block_off = (uint32_t)(offset % g_ext4.block_size);
        size_t         chunk = g_ext4.block_size - block_off;
        int            rc;

        if (chunk > count) chunk = count;
        rc = ext4_cache_get_mut(block_no, &block);
        if (rc < 0) return rc;

        e_memcpy(block + block_off, src, chunk);
        src += chunk;
        offset += chunk;
        count -= chunk;
    }

    return 0;
}

static int ext4_has_dirty_internal(void)
{
    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        if (g_ext4.blocks[i].valid && g_ext4.blocks[i].dirty)
            return 1;
    }
    for (uint32_t i = 0U; i < EXT4_INODE_CACHE_SLOTS; i++) {
        if (g_ext4.inodes[i].valid && g_ext4.inodes[i].dirty)
            return 1;
    }
    return 0;
}

static int ext4_write_superblock_cached(void)
{
    uint32_t crc;

    e_memcpy(g_ext4.sb_raw, &g_ext4.sb, sizeof(g_ext4.sb));
    crc = ext4_crc32c_raw(0xFFFFFFFFU, g_ext4.sb_raw, EXT4_SUPER_CSUM_OFF);
    *(uint32_t *)(void *)(g_ext4.sb_raw + EXT4_SUPER_CSUM_OFF) = crc;
    return ext4_write_bytes_cached(EXT4_SUPER_OFFSET, g_ext4.sb_raw, EXT4_SUPER_SIZE);
}

static uint64_t ext4_super_free_blocks(void)
{
    return (uint64_t)g_ext4.sb.s_free_blocks_count_lo |
           ((uint64_t)g_ext4.sb.s_free_blocks_count_hi << 32);
}

static void ext4_super_set_free_blocks(uint64_t v)
{
    g_ext4.sb.s_free_blocks_count_lo = (uint32_t)v;
    g_ext4.sb.s_free_blocks_count_hi = (uint32_t)(v >> 32);
}

static int ext4_read_group_desc_raw(uint32_t group, uint8_t *raw)
{
    uint64_t gdt_block;
    uint64_t offset;

    if (!raw) return -EFAULT;
    if (group >= g_ext4.group_count) return -EINVAL;

    gdt_block = (g_ext4.block_size == 1024U) ? 2ULL : 1ULL;
    offset = gdt_block * g_ext4.block_size +
             (uint64_t)group * g_ext4.desc_size;

    e_memset(raw, 0U, 64U);
    if (ext4_read_bytes(offset, raw, g_ext4.desc_size) < 0)
        return -EIO;
    return 0;
}

static int ext4_read_group_desc_uncached(uint32_t group, ext4_group_desc_t *out)
{
    uint8_t  block[EXT4_MAX_BLOCK_SIZE];
    uint8_t  block2[EXT4_MAX_BLOCK_SIZE];
    uint8_t  raw[64];
    uint64_t gdt_block;
    uint64_t offset;
    uint32_t block_no;
    uint32_t block_off;

    if (!out) return -EFAULT;
    if (group >= g_ext4.group_count) return -EINVAL;

    gdt_block = (g_ext4.block_size == 1024U) ? 2ULL : 1ULL;
    offset = gdt_block * g_ext4.block_size +
             (uint64_t)group * g_ext4.desc_size;
    block_no = (uint32_t)(offset / g_ext4.block_size);
    block_off = (uint32_t)(offset % g_ext4.block_size);

    if (ext4_raw_block_read(block_no, block) < 0)
        return -EIO;
    e_memset(raw, 0U, sizeof(raw));
    if (block_off + g_ext4.desc_size <= g_ext4.block_size) {
        e_memcpy(raw, block + block_off, g_ext4.desc_size);
    } else {
        uint32_t first = g_ext4.block_size - block_off;
        if (ext4_raw_block_read(block_no + 1U, block2) < 0)
            return -EIO;
        e_memcpy(raw, block + block_off, first);
        e_memcpy(raw + first, block2, g_ext4.desc_size - first);
    }
    e_memset(out, 0U, sizeof(*out));
    e_memcpy(out, raw,
             (g_ext4.desc_size < sizeof(*out)) ? g_ext4.desc_size : sizeof(*out));
    return 0;
}

static void ext4_inode_raw_update_checksum(uint32_t ino, uint8_t *raw, ext4_inode_t *inode)
{
    uint32_t crc;
    uint32_t gen = inode->i_generation;
    uint16_t old_lo = inode->i_checksum_lo;
    uint16_t old_hi = inode->i_checksum_hi;

    inode->i_checksum_lo = 0U;
    inode->i_checksum_hi = 0U;
    e_memcpy(raw, inode, sizeof(*inode));

    crc = ext4_crc32c_raw(g_ext4.checksum_seed, &ino, sizeof(ino));
    crc = ext4_crc32c_raw(crc, &gen, sizeof(gen));
    crc = ext4_crc32c_raw(crc, raw, g_ext4.inode_size);

    inode->i_checksum_lo = (uint16_t)crc;
    inode->i_checksum_hi = (uint16_t)(crc >> 16);
    e_memcpy(raw, inode, sizeof(*inode));

    (void)old_lo;
    (void)old_hi;
}

static int ext4_group_desc_writeback(uint32_t group, ext4_group_desc_t *gd)
{
    uint8_t  raw[64];
    uint8_t  tmp[64];
    uint64_t bb_block;
    uint64_t ib_block;
    const uint8_t *bitmap;
    uint32_t crc;
    uint32_t group_le = group;
    uint64_t gdt_block;
    uint64_t offset;

    if (!gd) return -EFAULT;

    bb_block = (uint64_t)gd->bg_block_bitmap_lo |
               ((uint64_t)gd->bg_block_bitmap_hi << 32);
    ib_block = (uint64_t)gd->bg_inode_bitmap_lo |
               ((uint64_t)gd->bg_inode_bitmap_hi << 32);

    if (ext4_cache_get(bb_block, &bitmap) < 0)
        return -EIO;
    crc = ext4_seeded_checksum(bitmap, g_ext4.block_size);
    gd->bg_block_bitmap_csum_lo = (uint16_t)crc;
    gd->bg_block_bitmap_csum_hi = (uint16_t)(crc >> 16);

    if (ext4_cache_get(ib_block, &bitmap) < 0)
        return -EIO;
    crc = ext4_seeded_checksum(bitmap, g_ext4.block_size);
    gd->bg_inode_bitmap_csum_lo = (uint16_t)crc;
    gd->bg_inode_bitmap_csum_hi = (uint16_t)(crc >> 16);

    e_memset(raw, 0U, sizeof(raw));
    e_memcpy(raw, gd, sizeof(*gd));
    e_memcpy(tmp, raw, sizeof(tmp));
    ((ext4_group_desc_t *)(void *)tmp)->bg_checksum = 0U;
    crc = ext4_crc32c_raw(g_ext4.checksum_seed, &group_le, sizeof(group_le));
    crc = ext4_crc32c_raw(crc, tmp, g_ext4.desc_size);
    gd->bg_checksum = (uint16_t)crc;
    e_memcpy(raw, gd, sizeof(*gd));

    gdt_block = (g_ext4.block_size == 1024U) ? 2ULL : 1ULL;
    offset = gdt_block * g_ext4.block_size +
             (uint64_t)group * g_ext4.desc_size;
    return ext4_write_bytes_cached(offset, raw, g_ext4.desc_size);
}

static int ext4_update_super_counts(int64_t blocks_delta, int32_t inodes_delta)
{
    uint64_t free_blocks = ext4_super_free_blocks();
    uint32_t free_inodes = g_ext4.sb.s_free_inodes_count;

    free_blocks = (uint64_t)((int64_t)free_blocks + blocks_delta);
    free_inodes = (uint32_t)((int32_t)free_inodes + inodes_delta);

    ext4_super_set_free_blocks(free_blocks);
    g_ext4.sb.s_free_inodes_count = free_inodes;
    g_ext4.sb.s_wtime = (uint32_t)(timer_now_ms() / 1000ULL);
    g_ext4.sb.s_state = EXT4_SUPER_STATE_CLEAN;
    return ext4_write_superblock_cached();
}

static int ext4_read_group_desc(uint32_t group, ext4_group_desc_t *out)
{
    uint8_t  raw[64];

    if (!out) return -EFAULT;
    if (g_ext4.desc_size > sizeof(raw)) return -EIO;
    if (ext4_read_group_desc_raw(group, raw) < 0)
        return -EIO;

    e_memset(out, 0, sizeof(*out));
    e_memcpy(out, raw,
             (g_ext4.desc_size < sizeof(*out)) ? g_ext4.desc_size : sizeof(*out));
    return 0;
}

static int ext4_load_inode_uncached_raw(uint32_t ino, uint8_t *raw, ext4_inode_t *out)
{
    ext4_group_desc_t gd;
    uint64_t          table_block;
    uint64_t          offset;
    uint32_t          group;
    uint32_t          index;

    if (!raw || !out) return -EFAULT;
    if (ino == 0U) return -EINVAL;

    group = (ino - 1U) / g_ext4.inodes_per_group;
    index = (ino - 1U) % g_ext4.inodes_per_group;

    if (ext4_read_group_desc_uncached(group, &gd) < 0)
        return -EIO;

    table_block = (uint64_t)gd.bg_inode_table_lo |
                  ((uint64_t)gd.bg_inode_table_hi << 32);
    offset = table_block * g_ext4.block_size +
             (uint64_t)index * g_ext4.inode_size;

    e_memset(raw, 0U, 512U);
    if (ext4_read_bytes(offset, raw, g_ext4.inode_size) < 0)
        return -EIO;

    e_memset(out, 0, sizeof(*out));
    e_memcpy(out, raw,
             (g_ext4.inode_size < sizeof(*out))
                 ? g_ext4.inode_size : sizeof(*out));
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
    if (victim->valid && victim->dirty) {
        if (ext4_commit_pending() < 0)
            return -EIO;
    }
    if (ext4_load_inode_uncached_raw(ino, victim->raw, &victim->inode) < 0)
        return -EIO;

    victim->valid = true;
    victim->dirty = false;
    victim->ino = ino;
    victim->stamp = ++g_ext4.clock;
    *out = victim->inode;
    return 0;
}

static int ext4_bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0U;
}

static void ext4_bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static void ext4_bitmap_clear(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

static uint32_t ext4_bitmap_last_set(const uint8_t *bitmap, uint32_t bits)
{
    while (bits > 0U) {
        uint32_t bit = bits - 1U;
        if (ext4_bitmap_test(bitmap, bit))
            return bit + 1U;
        bits--;
    }
    return 0U;
}

static int ext4_write_group_desc(uint32_t group, ext4_group_desc_t *gd)
{
    uint8_t  *bitmap;
    uint64_t iblock;
    uint32_t last_used;

    if (!gd) return -EFAULT;

    iblock = (uint64_t)gd->bg_inode_bitmap_lo |
             ((uint64_t)gd->bg_inode_bitmap_hi << 32);
    if (ext4_cache_get_mut(iblock, &bitmap) < 0)
        return -EIO;

    last_used = ext4_bitmap_last_set(bitmap, g_ext4.inodes_per_group);
    if (last_used <= g_ext4.inodes_per_group)
        ext4_group_set_itable_unused(gd, g_ext4.inodes_per_group - last_used);

    return ext4_group_desc_writeback(group, gd);
}

static void ext4_inode_init_extent_root(ext4_inode_t *inode)
{
    ext4_extent_header_t *hdr;

    e_memset(inode->i_block, 0U, sizeof(inode->i_block));
    hdr = (ext4_extent_header_t *)(void *)inode->i_block;
    hdr->eh_magic = EXT4_EXT_MAGIC;
    hdr->eh_entries = 0U;
    hdr->eh_max = (uint16_t)((sizeof(inode->i_block) - sizeof(*hdr)) / sizeof(ext4_extent_t));
    hdr->eh_depth = 0U;
    hdr->eh_generation = 0U;
}

static uint64_t ext4_inode_allocated_blocks(const ext4_inode_t *inode)
{
    if (ext4_inode_has_extents(inode)) {
        const ext4_extent_header_t *hdr = (const ext4_extent_header_t *)(const void *)inode->i_block;
        const ext4_extent_t *ext = (const ext4_extent_t *)(const void *)(inode->i_block + sizeof(*hdr));
        uint64_t total = 0ULL;

        if (hdr->eh_magic != EXT4_EXT_MAGIC || hdr->eh_depth != 0U)
            return 0ULL;
        for (uint16_t i = 0U; i < hdr->eh_entries; i++) {
            uint32_t len = ext[i].ee_len & 0x7FFFU;
            if (len == 0U) len = 32768U;
            total += len;
        }
        return total;
    } else {
        const uint32_t *ptrs = (const uint32_t *)(const void *)inode->i_block;
        uint64_t total = 0ULL;

        for (uint32_t i = 0U; i < 12U; i++) {
            if (ptrs[i] != 0U)
                total++;
        }
        if (ptrs[12] != 0U) {
            const uint8_t *blk;
            if (ext4_cache_get(ptrs[12], &blk) == 0) {
                const uint32_t *ind = (const uint32_t *)(const void *)blk;
                total++;
                for (uint32_t i = 0U; i < g_ext4.block_size / sizeof(uint32_t); i++) {
                    if (ind[i] != 0U)
                        total++;
                }
            }
        }
        return total;
    }
}

static void ext4_inode_refresh_blocks_field(ext4_inode_t *inode)
{
    uint64_t sectors = ext4_inode_allocated_blocks(inode) * g_ext4.sectors_per_block;
    inode->i_blocks_lo = (uint32_t)sectors;
    inode->i_blocks_high = (uint16_t)(sectors >> 32);
}

static int ext4_alloc_block_from_group(uint32_t group, uint64_t *block_out)
{
    ext4_group_desc_t gd;
    uint8_t          *bitmap;
    uint64_t          bb_block;
    uint64_t          first_block;
    uint32_t          limit;

    if (!block_out) return -EFAULT;
    if (ext4_read_group_desc(group, &gd) < 0)
        return -EIO;
    if (ext4_group_free_blocks(&gd) == 0U)
        return -ENOSPC;

    bb_block = (uint64_t)gd.bg_block_bitmap_lo |
               ((uint64_t)gd.bg_block_bitmap_hi << 32);
    if (ext4_cache_get_mut(bb_block, &bitmap) < 0)
        return -EIO;

    first_block = ext4_group_first_block(group);
    limit = g_ext4.blocks_per_group;
    if (first_block + limit > g_ext4.block_count)
        limit = (uint32_t)(g_ext4.block_count - first_block);

    for (uint32_t bit = 0U; bit < limit; bit++) {
        if (ext4_bitmap_test(bitmap, bit))
            continue;
        ext4_bitmap_set(bitmap, bit);
        ext4_group_set_free_blocks(&gd, ext4_group_free_blocks(&gd) - 1U);
        if (ext4_write_group_desc(group, &gd) < 0)
            return -EIO;
        if (ext4_update_super_counts(-1, 0) < 0)
            return -EIO;
        *block_out = first_block + bit;
        if (ext4_cache_get_mut(*block_out, &bitmap) < 0)
            return -EIO;
        e_memset(bitmap, 0U, g_ext4.block_size);
        return 0;
    }

    return -ENOSPC;
}

static int ext4_alloc_block_preferred(uint32_t preferred_group, uint64_t *block_out)
{
    for (uint32_t pass = 0U; pass < g_ext4.group_count; pass++) {
        uint32_t group = (preferred_group + pass) % g_ext4.group_count;
        int rc = ext4_alloc_block_from_group(group, block_out);
        if (rc == 0)
            return 0;
        if (rc != -ENOSPC)
            return rc;
    }
    return -ENOSPC;
}

static int ext4_free_block(uint64_t block_no)
{
    ext4_group_desc_t gd;
    uint8_t          *bitmap;
    uint64_t          bb_block;
    uint32_t          group = ext4_block_to_group(block_no);
    uint32_t          bit = ext4_block_group_bit(block_no);

    if (group >= g_ext4.group_count)
        return -EINVAL;
    if (ext4_read_group_desc(group, &gd) < 0)
        return -EIO;

    bb_block = (uint64_t)gd.bg_block_bitmap_lo |
               ((uint64_t)gd.bg_block_bitmap_hi << 32);
    if (ext4_cache_get_mut(bb_block, &bitmap) < 0)
        return -EIO;
    if (!ext4_bitmap_test(bitmap, bit))
        return 0;

    ext4_bitmap_clear(bitmap, bit);
    ext4_group_set_free_blocks(&gd, ext4_group_free_blocks(&gd) + 1U);
    if (ext4_write_group_desc(group, &gd) < 0)
        return -EIO;
    return ext4_update_super_counts(1, 0);
}

static int ext4_alloc_inode_preferred(uint32_t preferred_group, int is_dir, uint32_t *ino_out)
{
    if (!ino_out) return -EFAULT;

    for (uint32_t pass = 0U; pass < g_ext4.group_count; pass++) {
        uint32_t group = (preferred_group + pass) % g_ext4.group_count;
        ext4_group_desc_t gd;
        uint8_t          *bitmap;
        uint64_t          ib_block;

        if (ext4_read_group_desc(group, &gd) < 0)
            return -EIO;
        if (ext4_group_free_inodes(&gd) == 0U)
            continue;

        ib_block = (uint64_t)gd.bg_inode_bitmap_lo |
                   ((uint64_t)gd.bg_inode_bitmap_hi << 32);
        if (ext4_cache_get_mut(ib_block, &bitmap) < 0)
            return -EIO;

        for (uint32_t bit = 0U; bit < g_ext4.inodes_per_group; bit++) {
            uint32_t ino = group * g_ext4.inodes_per_group + bit + 1U;

            if ((ino < g_ext4.sb.s_first_ino && ino != EXT4_ROOT_INO) ||
                ext4_bitmap_test(bitmap, bit))
                continue;

            ext4_bitmap_set(bitmap, bit);
            ext4_group_set_free_inodes(&gd, ext4_group_free_inodes(&gd) - 1U);
            if (is_dir)
                ext4_group_set_used_dirs(&gd, ext4_group_used_dirs(&gd) + 1U);
            if (ext4_write_group_desc(group, &gd) < 0)
                return -EIO;
            if (ext4_update_super_counts(0, -1) < 0)
                return -EIO;
            *ino_out = ino;
            return 0;
        }
    }

    return -ENOSPC;
}

static int ext4_free_inode_bitmap(uint32_t ino, int is_dir)
{
    ext4_group_desc_t gd;
    uint8_t          *bitmap;
    uint64_t          ib_block;
    uint32_t          group;
    uint32_t          bit;

    if (ino == 0U) return -EINVAL;
    group = (ino - 1U) / g_ext4.inodes_per_group;
    bit = (ino - 1U) % g_ext4.inodes_per_group;

    if (ext4_read_group_desc(group, &gd) < 0)
        return -EIO;
    ib_block = (uint64_t)gd.bg_inode_bitmap_lo |
               ((uint64_t)gd.bg_inode_bitmap_hi << 32);
    if (ext4_cache_get_mut(ib_block, &bitmap) < 0)
        return -EIO;
    if (!ext4_bitmap_test(bitmap, bit))
        return 0;

    ext4_bitmap_clear(bitmap, bit);
    ext4_group_set_free_inodes(&gd, ext4_group_free_inodes(&gd) + 1U);
    if (is_dir && ext4_group_used_dirs(&gd) > 0U)
        ext4_group_set_used_dirs(&gd, ext4_group_used_dirs(&gd) - 1U);
    if (ext4_write_group_desc(group, &gd) < 0)
        return -EIO;
    return ext4_update_super_counts(0, 1);
}

static int ext4_extent_allocate_lblock(uint32_t ino, ext4_inode_t *inode,
                                       uint32_t lblock, uint64_t *pblock_out)
{
    ext4_extent_header_t *hdr;
    ext4_extent_t        *ext;
    uint64_t              phys = 0ULL;
    uint32_t              preferred_group;
    int                   insert = 0;
    int                   rc;

    if (!inode || !pblock_out) return -EFAULT;

    hdr = (ext4_extent_header_t *)(void *)inode->i_block;
    if (hdr->eh_magic != EXT4_EXT_MAGIC)
        return -EIO;
    if (hdr->eh_depth != 0U)
        return -ENOSYS;

    ext = (ext4_extent_t *)(void *)(inode->i_block + sizeof(*hdr));
    for (uint16_t i = 0U; i < hdr->eh_entries; i++) {
        uint32_t len = ext[i].ee_len & 0x7FFFU;
        if (len == 0U) len = 32768U;
        if (lblock < ext[i].ee_block)
            break;
        insert = (int)i + 1;
        if (lblock < ext[i].ee_block + len) {
            uint64_t base = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
            *pblock_out = base + (lblock - ext[i].ee_block);
            return 0;
        }
    }

    preferred_group = (ino - 1U) / g_ext4.inodes_per_group;
    rc = ext4_alloc_block_preferred(preferred_group, &phys);
    if (rc < 0) return rc;

    if (insert > 0) {
        ext4_extent_t *prev = &ext[insert - 1];
        uint32_t prev_len = prev->ee_len & 0x7FFFU;
        if (prev_len == 0U) prev_len = 32768U;
        if (prev->ee_block + prev_len == lblock) {
            uint64_t prev_phys = ((uint64_t)prev->ee_start_hi << 32) | prev->ee_start_lo;
            if (prev_phys + prev_len == phys && prev_len < 32768U) {
                prev->ee_len = (uint16_t)(prev_len + 1U);
                ext4_inode_refresh_blocks_field(inode);
                *pblock_out = phys;
                return 0;
            }
        }
    }

    if (hdr->eh_entries >= hdr->eh_max) {
        (void)ext4_free_block(phys);
        return -ENOSPC;
    }

    if ((uint16_t)insert < hdr->eh_entries) {
        e_memmove(&ext[insert + 1], &ext[insert],
                  (size_t)(hdr->eh_entries - (uint16_t)insert) * sizeof(ext4_extent_t));
    }
    ext[insert].ee_block = lblock;
    ext[insert].ee_len = 1U;
    ext[insert].ee_start_hi = (uint16_t)(phys >> 32);
    ext[insert].ee_start_lo = (uint32_t)phys;
    hdr->eh_entries++;
    ext4_inode_refresh_blocks_field(inode);
    *pblock_out = phys;
    return 0;
}

static int ext4_legacy_allocate_lblock(ext4_inode_t *inode, uint32_t lblock,
                                       uint64_t *pblock_out)
{
    uint32_t *ptrs = (uint32_t *)(void *)inode->i_block;
    uint32_t preferred_group = EXT4_ROOT_INO - 1U;
    uint64_t phys;
    int      rc;

    if (!inode || !pblock_out) return -EFAULT;

    if (lblock < 12U) {
        if (ptrs[lblock] != 0U) {
            *pblock_out = ptrs[lblock];
            return 0;
        }
        rc = ext4_alloc_block_preferred(preferred_group, &phys);
        if (rc < 0) return rc;
        ptrs[lblock] = (uint32_t)phys;
        ext4_inode_refresh_blocks_field(inode);
        *pblock_out = phys;
        return 0;
    }

    lblock -= 12U;
    if (lblock >= g_ext4.block_size / sizeof(uint32_t))
        return -ENOSYS;

    if (ptrs[12] == 0U) {
        uint8_t *ind_block;
        rc = ext4_alloc_block_preferred(preferred_group, &phys);
        if (rc < 0) return rc;
        ptrs[12] = (uint32_t)phys;
        if (ext4_cache_get_mut(phys, &ind_block) < 0)
            return -EIO;
        e_memset(ind_block, 0U, g_ext4.block_size);
    }

    {
        uint8_t *blk;
        uint32_t *ind;

        if (ext4_cache_get_mut(ptrs[12], &blk) < 0)
            return -EIO;
        ind = (uint32_t *)(void *)blk;
        if (ind[lblock] == 0U) {
            rc = ext4_alloc_block_preferred(preferred_group, &phys);
            if (rc < 0) return rc;
            ind[lblock] = (uint32_t)phys;
        } else {
            phys = ind[lblock];
        }
    }

    ext4_inode_refresh_blocks_field(inode);
    *pblock_out = phys;
    return 0;
}

static int ext4_ensure_lblock_allocated(uint32_t ino, ext4_inode_t *inode,
                                        uint32_t lblock, uint64_t *pblock_out)
{
    int rc;

    rc = ext4_map_lblock(inode, lblock, pblock_out);
    if (rc < 0) return rc;
    if (*pblock_out != 0ULL)
        return 0;

    if (ext4_inode_has_extents(inode))
        return ext4_extent_allocate_lblock(ino, inode, lblock, pblock_out);

    return ext4_legacy_allocate_lblock(inode, lblock, pblock_out);
}

static int ext4_free_inode_all_blocks(ext4_inode_t *inode)
{
    if (ext4_inode_has_extents(inode)) {
        ext4_extent_header_t *hdr = (ext4_extent_header_t *)(void *)inode->i_block;
        ext4_extent_t *ext = (ext4_extent_t *)(void *)(inode->i_block + sizeof(*hdr));

        if (hdr->eh_magic != EXT4_EXT_MAGIC || hdr->eh_depth != 0U)
            return -ENOSYS;
        for (uint16_t i = 0U; i < hdr->eh_entries; i++) {
            uint32_t len = ext[i].ee_len & 0x7FFFU;
            uint64_t phys = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
            if (len == 0U) len = 32768U;
            for (uint32_t j = 0U; j < len; j++) {
                int rc = ext4_free_block(phys + j);
                if (rc < 0) return rc;
            }
        }
        ext4_inode_init_extent_root(inode);
    } else {
        uint32_t *ptrs = (uint32_t *)(void *)inode->i_block;

        for (uint32_t i = 0U; i < 12U; i++) {
            if (ptrs[i] != 0U) {
                int rc = ext4_free_block(ptrs[i]);
                if (rc < 0) return rc;
                ptrs[i] = 0U;
            }
        }
        if (ptrs[12] != 0U) {
            const uint8_t *blk;
            if (ext4_cache_get(ptrs[12], &blk) < 0)
                return -EIO;
            for (uint32_t i = 0U; i < g_ext4.block_size / sizeof(uint32_t); i++) {
                uint32_t entry = ((const uint32_t *)(const void *)blk)[i];
                if (entry != 0U) {
                    int rc = ext4_free_block(entry);
                    if (rc < 0) return rc;
                }
            }
            if (ext4_free_block(ptrs[12]) < 0)
                return -EIO;
            ptrs[12] = 0U;
        }
    }

    ext4_inode_refresh_blocks_field(inode);
    return 0;
}

static int ext4_shrink_inode_blocks(ext4_inode_t *inode, uint32_t keep_blocks)
{
    if (ext4_inode_has_extents(inode)) {
        ext4_extent_header_t *hdr = (ext4_extent_header_t *)(void *)inode->i_block;
        ext4_extent_t *ext = (ext4_extent_t *)(void *)(inode->i_block + sizeof(*hdr));
        uint16_t i = 0U;

        if (hdr->eh_magic != EXT4_EXT_MAGIC || hdr->eh_depth != 0U)
            return -ENOSYS;

        while (i < hdr->eh_entries) {
            uint32_t start = ext[i].ee_block;
            uint32_t len = ext[i].ee_len & 0x7FFFU;
            uint64_t phys = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;

            if (len == 0U) len = 32768U;
            if (keep_blocks <= start) {
                for (uint32_t j = 0U; j < len; j++) {
                    int rc = ext4_free_block(phys + j);
                    if (rc < 0) return rc;
                }
                if (i + 1U < hdr->eh_entries) {
                    e_memmove(&ext[i], &ext[i + 1],
                              (size_t)(hdr->eh_entries - i - 1U) * sizeof(ext4_extent_t));
                }
                hdr->eh_entries--;
                continue;
            }
            if (keep_blocks < start + len) {
                uint32_t keep_len = keep_blocks - start;
                for (uint32_t j = keep_len; j < len; j++) {
                    int rc = ext4_free_block(phys + j);
                    if (rc < 0) return rc;
                }
                ext[i].ee_len = (uint16_t)keep_len;
            }
            i++;
        }
    } else {
        uint32_t *ptrs = (uint32_t *)(void *)inode->i_block;

        for (uint32_t i = keep_blocks; i < 12U; i++) {
            if (ptrs[i] != 0U) {
                int rc = ext4_free_block(ptrs[i]);
                if (rc < 0) return rc;
                ptrs[i] = 0U;
            }
        }

        if (ptrs[12] != 0U) {
            uint8_t *blk;
            uint32_t *ind;
            uint32_t start = (keep_blocks > 12U) ? (keep_blocks - 12U) : 0U;
            int any = 0;

            if (ext4_cache_get_mut(ptrs[12], &blk) < 0)
                return -EIO;
            ind = (uint32_t *)(void *)blk;
            for (uint32_t i = start; i < g_ext4.block_size / sizeof(uint32_t); i++) {
                if (ind[i] != 0U) {
                    int rc = ext4_free_block(ind[i]);
                    if (rc < 0) return rc;
                    ind[i] = 0U;
                }
            }
            for (uint32_t i = 0U; i < g_ext4.block_size / sizeof(uint32_t); i++) {
                if (ind[i] != 0U) {
                    any = 1;
                    break;
                }
            }
            if (!any) {
                if (ext4_free_block(ptrs[12]) < 0)
                    return -EIO;
                ptrs[12] = 0U;
            }
        }
    }

    ext4_inode_refresh_blocks_field(inode);
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

static int ext4_zero_inode_range(uint32_t ino, ext4_inode_t *inode, uint64_t pos, uint64_t count)
{
    while (count > 0ULL) {
        uint32_t lblock = (uint32_t)(pos / g_ext4.block_size);
        uint32_t block_off = (uint32_t)(pos % g_ext4.block_size);
        size_t   chunk = g_ext4.block_size - block_off;
        uint64_t pblock = 0ULL;
        uint8_t *dst;
        int      rc;

        if ((uint64_t)chunk > count)
            chunk = (size_t)count;

        rc = ext4_ensure_lblock_allocated(ino, inode, lblock, &pblock);
        if (rc < 0) return rc;

        rc = ext4_cache_get_mut(pblock, &dst);
        if (rc < 0) return rc;

        e_memset(dst + block_off, 0U, chunk);
        pos += chunk;
        count -= chunk;
    }

    return 0;
}

static int ext4_write_inode_data(uint32_t ino, ext4_inode_t *inode, uint64_t pos,
                                 const void *buf, size_t count, size_t *written_out)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t       size;
    uint64_t       capacity;
    size_t         written = 0U;

    if (!inode) return -EFAULT;
    if (!buf && count != 0U) return -EFAULT;
    if (count == 0U) {
        if (written_out) *written_out = 0U;
        return 0;
    }

    size = ext4_inode_size(inode);
    capacity = ext4_inode_capacity_bytes(inode);
    if (pos > capacity) return -ENOSPC;
    if ((uint64_t)count > capacity - pos)
        count = (size_t)(capacity - pos);
    if (count == 0U) return -ENOSPC;

    while (count > 0U) {
        uint32_t lblock = (uint32_t)(pos / g_ext4.block_size);
        uint32_t block_off = (uint32_t)(pos % g_ext4.block_size);
        size_t   chunk = g_ext4.block_size - block_off;
        uint64_t pblock = 0ULL;
        uint8_t *dst;
        int      rc;

        if (chunk > count) chunk = count;

        rc = ext4_ensure_lblock_allocated(ino, inode, lblock, &pblock);
        if (rc < 0) return rc;

        rc = ext4_cache_get_mut(pblock, &dst);
        if (rc < 0) return rc;
        e_memcpy(dst + block_off, src, chunk);

        src += chunk;
        pos += chunk;
        count -= chunk;
        written += chunk;
    }

    if (pos > size)
        ext4_inode_set_size(inode, pos);
    inode->i_mtime = (uint32_t)(timer_now_ms() / 1000ULL);
    inode->i_ctime = inode->i_mtime;

    if (ext4_mark_inode_dirty(ino, inode) < 0)
        return -EIO;

    if (written_out) *written_out = written;
    return 0;
}

static int ext4_truncate_inode(uint32_t ino, ext4_inode_t *inode, uint64_t new_size)
{
    uint64_t old_size;
    uint64_t capacity;
    uint32_t keep_blocks;
    int      rc;

    if (!inode) return -EFAULT;

    old_size = ext4_inode_size(inode);
    capacity = ext4_inode_capacity_bytes(inode);
    if (new_size > capacity)
        return -ENOSPC;

    if (new_size < old_size) {
        uint32_t block_off = (uint32_t)(new_size % g_ext4.block_size);
        if (block_off != 0U) {
            rc = ext4_zero_inode_range(ino, inode, new_size,
                                       (uint64_t)(g_ext4.block_size - block_off));
            if (rc < 0) return rc;
        }
        keep_blocks = (uint32_t)((new_size + g_ext4.block_size - 1ULL) / g_ext4.block_size);
        rc = ext4_shrink_inode_blocks(inode, keep_blocks);
        if (rc < 0) return rc;
    } else if (new_size > old_size) {
        rc = ext4_zero_inode_range(ino, inode, old_size, new_size - old_size);
        if (rc < 0) return rc;
    }

    ext4_inode_set_size(inode, new_size);
    inode->i_mtime = (uint32_t)(timer_now_ms() / 1000ULL);
    inode->i_ctime = inode->i_mtime;
    return ext4_mark_inode_dirty(ino, inode);
}

static int ext4_txn_find_block(const ext4_journal_txn_t *txn, uint64_t block_no)
{
    for (uint32_t i = 0U; i < txn->count; i++) {
        if (txn->entries[i].target_block == block_no)
            return (int)i;
    }
    return -1;
}

static int ext4_txn_get_or_load(ext4_journal_txn_t *txn, uint64_t block_no, uint8_t **buf_out)
{
    int idx;

    if (!txn || !buf_out) return -EFAULT;

    idx = ext4_txn_find_block(txn, block_no);
    if (idx >= 0) {
        *buf_out = txn->data[idx];
        return idx;
    }

    if (txn->count >= EXT4_JOURNAL_MAX_TX_BLOCKS)
        return -ENOSPC;

    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        if (g_ext4.blocks[i].valid && g_ext4.blocks[i].block_no == block_no) {
            e_memcpy(txn->data[txn->count], g_ext4.blocks[i].data, g_ext4.block_size);
            goto added;
        }
    }

    if (ext4_raw_block_read(block_no, txn->data[txn->count]) < 0)
        return -EIO;

added:
    txn->entries[txn->count].target_block = block_no;
    txn->entries[txn->count].data_crc = 0U;
    txn->entries[txn->count].reserved = 0U;
    *buf_out = txn->data[txn->count];
    txn->count++;
    return (int)(txn->count - 1U);
}

static int ext4_txn_apply_inode_raw(ext4_journal_txn_t *txn, uint64_t offset,
                                    const uint8_t *raw, size_t raw_len)
{
    size_t done = 0U;

    while (done < raw_len) {
        uint64_t block_no = offset / g_ext4.block_size;
        uint32_t block_off = (uint32_t)(offset % g_ext4.block_size);
        size_t   chunk = g_ext4.block_size - block_off;
        uint8_t *dst;
        int      rc;

        if (chunk > raw_len - done)
            chunk = raw_len - done;

        rc = ext4_txn_get_or_load(txn, block_no, &dst);
        if (rc < 0) return rc;
        e_memcpy(dst + block_off, raw + done, chunk);

        offset += chunk;
        done += chunk;
    }

    return 0;
}

static int ext4_txn_add_dirty_inode(ext4_journal_txn_t *txn, ext4_inode_cache_t *slot)
{
    ext4_group_desc_t gd;
    uint64_t          table_block;
    uint64_t          offset;
    uint32_t          group;
    uint32_t          index;
    size_t            raw_len;

    if (!txn || !slot) return -EFAULT;
    if (!slot->valid || !slot->dirty) return 0;

    group = (slot->ino - 1U) / g_ext4.inodes_per_group;
    index = (slot->ino - 1U) % g_ext4.inodes_per_group;

    if (ext4_read_group_desc(group, &gd) < 0)
        return -EIO;

    table_block = (uint64_t)gd.bg_inode_table_lo |
                  ((uint64_t)gd.bg_inode_table_hi << 32);
    offset = table_block * g_ext4.block_size +
             (uint64_t)index * g_ext4.inode_size;
    raw_len = (g_ext4.inode_size < sizeof(slot->raw))
            ? g_ext4.inode_size : sizeof(slot->raw);

    ext4_inode_raw_update_checksum(slot->ino, slot->raw, &slot->inode);
    return ext4_txn_apply_inode_raw(txn, offset, slot->raw, raw_len);
}

static int ext4_txn_build(ext4_journal_txn_t *txn)
{
    if (!txn) return -EFAULT;
    e_memset(txn, 0U, sizeof(*txn));

    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        int idx;

        if (!g_ext4.blocks[i].valid || !g_ext4.blocks[i].dirty)
            continue;

        idx = ext4_txn_find_block(txn, g_ext4.blocks[i].block_no);
        if (idx < 0) {
            uint8_t *dst;
            idx = ext4_txn_get_or_load(txn, g_ext4.blocks[i].block_no, &dst);
            if (idx < 0) return idx;
        }
        e_memcpy(txn->data[idx], g_ext4.blocks[i].data, g_ext4.block_size);
    }

    for (uint32_t i = 0U; i < EXT4_INODE_CACHE_SLOTS; i++) {
        int rc = ext4_txn_add_dirty_inode(txn, &g_ext4.inodes[i]);
        if (rc < 0) return rc;
    }

    return 0;
}

static void ext4_txn_clear_dirty_flags(void)
{
    for (uint32_t i = 0U; i < EXT4_BLOCK_CACHE_SLOTS; i++) {
        if (g_ext4.blocks[i].valid)
            g_ext4.blocks[i].dirty = false;
    }
    for (uint32_t i = 0U; i < EXT4_INODE_CACHE_SLOTS; i++) {
        if (g_ext4.inodes[i].valid)
            g_ext4.inodes[i].dirty = false;
    }
}

static int ext4_txn_apply_to_disk(const ext4_journal_txn_t *txn)
{
    if (!txn) return -EFAULT;

    for (uint32_t i = 0U; i < txn->count; i++) {
        if (ext4_raw_block_write(txn->entries[i].target_block, txn->data[i]) < 0)
            return -EIO;
        for (uint32_t j = 0U; j < EXT4_BLOCK_CACHE_SLOTS; j++) {
            if (g_ext4.blocks[j].valid &&
                g_ext4.blocks[j].block_no == txn->entries[i].target_block) {
                e_memcpy(g_ext4.blocks[j].data, txn->data[i], g_ext4.block_size);
                g_ext4.blocks[j].dirty = false;
            }
        }
    }

    return (blk_flush_sync() == BLK_OK) ? 0 : -EIO;
}

static void ext4_journal_pack_header(uint8_t *block_buf, const ext4_journal_txn_t *txn,
                                     uint32_t state)
{
    ext4_journal_header_t *hdr = (ext4_journal_header_t *)(void *)block_buf;

    e_memset(block_buf, 0U, g_ext4.block_size);
    hdr->magic = EXT4_JOURNAL_MAGIC;
    hdr->version = EXT4_JOURNAL_VERSION;
    hdr->state = state;
    hdr->entry_count = txn->count;
    hdr->block_size = g_ext4.block_size;
    hdr->sequence = ++g_ext4.journal.sequence;
    hdr->header_crc = 0U;

    if (txn->count != 0U) {
        e_memcpy(block_buf + sizeof(*hdr), txn->entries,
                 txn->count * sizeof(ext4_journal_entry_t));
    }
    hdr->header_crc = ext4_crc32c_raw(0xFFFFFFFFU, block_buf, g_ext4.block_size);
}

static int ext4_journal_clear_disk(void)
{
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];

    if (!g_ext4.journal.ready)
        return 0;

    e_memset(block_buf, 0U, g_ext4.block_size);
    if (ext4_raw_block_write(g_ext4.journal.phys_blocks[0], block_buf) < 0)
        return -EIO;
    return (blk_flush_sync() == BLK_OK) ? 0 : -EIO;
}

static int ext4_journal_write_txn(ext4_journal_txn_t *txn)
{
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];

    if (!txn) return -EFAULT;
    if (!g_ext4.journal.ready) return -EIO;
    if (txn->count > EXT4_JOURNAL_MAX_TX_BLOCKS)
        return -ENOSPC;

    for (uint32_t i = 0U; i < txn->count; i++) {
        txn->entries[i].data_crc = ext4_journal_block_crc(txn->data[i]);
        if (ext4_raw_block_write(g_ext4.journal.phys_blocks[i + 1U], txn->data[i]) < 0)
            return -EIO;
    }

    ext4_journal_pack_header(block_buf, txn, EXT4_JOURNAL_STATE_PENDING);
    if (ext4_raw_block_write(g_ext4.journal.phys_blocks[0], block_buf) < 0)
        return -EIO;
    return (blk_flush_sync() == BLK_OK) ? 0 : -EIO;
}

static int ext4_sync_raw(void)
{
    int rc;

    if (!g_ext4.mounted)
        return -EIO;
    if (!ext4_has_dirty_internal())
        return (blk_flush_sync() == BLK_OK) ? 0 : -EIO;

    rc = ext4_txn_build(&g_ext4_txn);
    if (rc < 0) return rc;
    rc = ext4_txn_apply_to_disk(&g_ext4_txn);
    if (rc < 0) return rc;
    ext4_txn_clear_dirty_flags();
    return 0;
}

static int ext4_commit_pending(void)
{
    int rc;

    if (!g_ext4.mounted)
        return -EIO;
    if (!ext4_has_dirty_internal())
        return (blk_flush_sync() == BLK_OK) ? 0 : -EIO;
    if (g_ext4.journal.in_commit)
        return -EIO;

    rc = ext4_txn_build(&g_ext4_txn);
    if (rc < 0) return rc;

    g_ext4.journal.in_commit = true;
    if (g_ext4.journal.ready) {
        rc = ext4_journal_write_txn(&g_ext4_txn);
        if (rc < 0) goto out;
    }

    rc = ext4_txn_apply_to_disk(&g_ext4_txn);
    if (rc < 0) goto out;

    if (g_ext4.journal.ready) {
        rc = ext4_journal_clear_disk();
        if (rc < 0) goto out;
    }

    ext4_txn_clear_dirty_flags();
    rc = 0;

out:
    g_ext4.journal.in_commit = false;
    return rc;
}

int ext4_sync(void)
{
    return ext4_commit_pending();
}

static uint8_t ext4_inode_file_type(const ext4_inode_t *inode)
{
    if (!inode) return EXT4_FT_UNKNOWN;
    switch (inode->i_mode & S_IFMT) {
    case S_IFREG: return EXT4_FT_REG_FILE;
    case S_IFDIR: return EXT4_FT_DIR;
    case S_IFCHR: return EXT4_FT_CHRDEV;
    case S_IFLNK: return EXT4_FT_SYMLINK;
    default:      return EXT4_FT_UNKNOWN;
    }
}

static int ext4_dir_update_checksum(uint32_t dir_ino, const ext4_inode_t *dir_inode,
                                    uint64_t pblock)
{
    ext4_dir_entry_tail_t *tail;
    uint8_t               *block;
    uint32_t               crc;
    uint32_t               gen;

    if (!g_ext4.metadata_csum)
        return 0;
    if (!dir_inode) return -EFAULT;
    if (ext4_cache_get_mut(pblock, &block) < 0)
        return -EIO;

    tail = (ext4_dir_entry_tail_t *)(void *)(block + g_ext4.block_size - EXT4_DIR_TAIL_SIZE);
    tail->reserved_zero1 = 0U;
    tail->rec_len = EXT4_DIR_TAIL_SIZE;
    tail->reserved_zero2 = 0U;
    tail->reserved_ft = 0xDEU;
    tail->checksum = 0U;

    gen = dir_inode->i_generation;
    crc = ext4_crc32c_raw(g_ext4.checksum_seed, &dir_ino, sizeof(dir_ino));
    crc = ext4_crc32c_raw(crc, &gen, sizeof(gen));
    crc = ext4_crc32c_raw(crc, block, g_ext4.block_size - EXT4_DIR_TAIL_SIZE);
    tail->checksum = crc;
    return 0;
}

static int ext4_dir_init_empty_block(uint32_t dir_ino, const ext4_inode_t *dir_inode,
                                     uint64_t pblock)
{
    ext4_dir_entry_2_t *de;
    uint8_t            *block;

    if (ext4_cache_get_mut(pblock, &block) < 0)
        return -EIO;
    e_memset(block, 0U, g_ext4.block_size);

    de = (ext4_dir_entry_2_t *)(void *)block;
    de->inode = 0U;
    de->rec_len = (uint16_t)(g_ext4.block_size - EXT4_DIR_TAIL_SIZE);
    de->name_len = 0U;
    de->file_type = EXT4_FT_UNKNOWN;
    return ext4_dir_update_checksum(dir_ino, dir_inode, pblock);
}

static int ext4_dir_init_new_dir_block(uint32_t dir_ino, ext4_inode_t *dir_inode,
                                       uint32_t parent_ino, uint64_t pblock)
{
    ext4_dir_entry_2_t *dot;
    ext4_dir_entry_2_t *dotdot;
    ext4_dir_entry_2_t *free_de;
    uint8_t            *block;

    if (ext4_cache_get_mut(pblock, &block) < 0)
        return -EIO;
    e_memset(block, 0U, g_ext4.block_size);

    dot = (ext4_dir_entry_2_t *)(void *)block;
    dot->inode = dir_ino;
    dot->rec_len = ext4_dir_rec_len(1U);
    dot->name_len = 1U;
    dot->file_type = EXT4_FT_DIR;
    dot->name[0] = '.';

    dotdot = (ext4_dir_entry_2_t *)(void *)(block + dot->rec_len);
    dotdot->inode = parent_ino;
    dotdot->rec_len = ext4_dir_rec_len(2U);
    dotdot->name_len = 2U;
    dotdot->file_type = EXT4_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    free_de = (ext4_dir_entry_2_t *)(void *)(block + dot->rec_len + dotdot->rec_len);
    free_de->inode = 0U;
    free_de->rec_len = (uint16_t)(g_ext4.block_size - dot->rec_len - dotdot->rec_len -
                                  EXT4_DIR_TAIL_SIZE);
    free_de->name_len = 0U;
    free_de->file_type = EXT4_FT_UNKNOWN;

    ext4_inode_set_size(dir_inode, g_ext4.block_size);
    ext4_inode_refresh_blocks_field(dir_inode);
    return ext4_dir_update_checksum(dir_ino, dir_inode, pblock);
}

static int ext4_dir_is_tail(const ext4_dir_entry_2_t *de)
{
    return de->inode == 0U &&
           de->rec_len == EXT4_DIR_TAIL_SIZE &&
           de->name_len == 0U &&
           de->file_type == 0xDEU;
}

static int ext4_dir_add_entry(uint32_t dir_ino, ext4_inode_t *dir_inode,
                              uint32_t child_ino, uint8_t child_ft,
                              const char *name, uint16_t name_len)
{
    uint16_t need;
    uint64_t dir_size;

    if (!dir_inode || !name || name_len == 0U || name_len > EXT4_NAME_MAX)
        return -EINVAL;
    if (dir_inode->i_flags & EXT4_INODE_FLAG_INDEX)
        return -ENOSYS;

    need = ext4_dir_rec_len(name_len);
    dir_size = ext4_inode_size(dir_inode);

    for (uint32_t lblock = 0U; lblock < (uint32_t)(dir_size / g_ext4.block_size); lblock++) {
        uint64_t pblock = 0ULL;
        uint8_t *block;
        int rc = ext4_map_lblock(dir_inode, lblock, &pblock);
        if (rc < 0) return rc;
        if (pblock == 0ULL)
            continue;
        if (ext4_cache_get_mut(pblock, &block) < 0)
            return -EIO;

        for (uint32_t off = 0U; off + 8U <= g_ext4.block_size; ) {
            ext4_dir_entry_2_t *de = (ext4_dir_entry_2_t *)(void *)(block + off);
            uint16_t rec_len = de->rec_len;
            uint16_t actual_len;
            uint32_t slot_off;
            uint16_t slot_len;
            ext4_dir_entry_2_t *new_de;

            if (rec_len < 8U || (rec_len & 3U) != 0U || off + rec_len > g_ext4.block_size)
                return -EIO;
            if (ext4_dir_is_tail(de))
                break;

            if (de->inode == 0U) {
                actual_len = 0U;
                slot_off = off;
                slot_len = rec_len;
            } else {
                actual_len = ext4_dir_rec_len(de->name_len);
                if (rec_len < actual_len) return -EIO;
                if ((uint16_t)(rec_len - actual_len) < need) {
                    off += rec_len;
                    continue;
                }
                de->rec_len = actual_len;
                slot_off = off + actual_len;
                slot_len = (uint16_t)(rec_len - actual_len);
            }

            if (slot_len < need) {
                off += rec_len;
                continue;
            }

            new_de = (ext4_dir_entry_2_t *)(void *)(block + slot_off);
            new_de->inode = child_ino;
            new_de->name_len = (uint8_t)name_len;
            new_de->file_type = child_ft;
            e_memcpy(new_de->name, name, name_len);

            if ((uint16_t)(slot_len - need) >= 8U) {
                ext4_dir_entry_2_t *free_de = (ext4_dir_entry_2_t *)(void *)(block + slot_off + need);
                new_de->rec_len = need;
                free_de->inode = 0U;
                free_de->rec_len = (uint16_t)(slot_len - need);
                free_de->name_len = 0U;
                free_de->file_type = EXT4_FT_UNKNOWN;
            } else {
                new_de->rec_len = slot_len;
            }

            dir_inode->i_mtime = (uint32_t)(timer_now_ms() / 1000ULL);
            dir_inode->i_ctime = dir_inode->i_mtime;
            if (ext4_dir_update_checksum(dir_ino, dir_inode, pblock) < 0)
                return -EIO;
            return 0;
        }
    }

    {
        uint32_t lblock = (uint32_t)(dir_size / g_ext4.block_size);
        uint64_t pblock = 0ULL;
        int rc = ext4_ensure_lblock_allocated(dir_ino, dir_inode, lblock, &pblock);
        if (rc < 0) return rc;
        if (ext4_dir_init_empty_block(dir_ino, dir_inode, pblock) < 0)
            return -EIO;
        ext4_inode_set_size(dir_inode, dir_size + g_ext4.block_size);
        dir_inode->i_mtime = (uint32_t)(timer_now_ms() / 1000ULL);
        dir_inode->i_ctime = dir_inode->i_mtime;
        return ext4_dir_add_entry(dir_ino, dir_inode, child_ino, child_ft, name, name_len);
    }
}

static int ext4_dir_remove_entry(uint32_t dir_ino, ext4_inode_t *dir_inode,
                                 const char *name, uint16_t name_len,
                                 uint32_t *child_ino_out, uint8_t *child_ft_out)
{
    uint64_t dir_size;

    if (!dir_inode || !name || name_len == 0U)
        return -EINVAL;

    dir_size = ext4_inode_size(dir_inode);
    for (uint32_t lblock = 0U; lblock < (uint32_t)(dir_size / g_ext4.block_size); lblock++) {
        uint64_t pblock = 0ULL;
        uint8_t *block;
        uint32_t prev_off = 0xFFFFFFFFU;
        int rc = ext4_map_lblock(dir_inode, lblock, &pblock);

        if (rc < 0) return rc;
        if (pblock == 0ULL)
            continue;
        if (ext4_cache_get_mut(pblock, &block) < 0)
            return -EIO;

        for (uint32_t off = 0U; off + 8U <= g_ext4.block_size; ) {
            ext4_dir_entry_2_t *de = (ext4_dir_entry_2_t *)(void *)(block + off);
            uint16_t rec_len = de->rec_len;

            if (rec_len < 8U || (rec_len & 3U) != 0U || off + rec_len > g_ext4.block_size)
                return -EIO;
            if (ext4_dir_is_tail(de))
                break;

            if (de->inode != 0U && e_nameeq(de->name, de->name_len, name, name_len)) {
                uint32_t child_ino = de->inode;
                uint8_t  child_ft = de->file_type;

                if (prev_off != 0xFFFFFFFFU) {
                    ext4_dir_entry_2_t *prev = (ext4_dir_entry_2_t *)(void *)(block + prev_off);
                    prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                } else {
                    de->inode = 0U;
                    de->name_len = 0U;
                    de->file_type = EXT4_FT_UNKNOWN;
                }

                dir_inode->i_mtime = (uint32_t)(timer_now_ms() / 1000ULL);
                dir_inode->i_ctime = dir_inode->i_mtime;
                if (ext4_dir_update_checksum(dir_ino, dir_inode, pblock) < 0)
                    return -EIO;
                if (child_ino_out) *child_ino_out = child_ino;
                if (child_ft_out) *child_ft_out = child_ft;
                return 0;
            }

            prev_off = off;
            off += rec_len;
        }
    }

    return -ENOENT;
}

static int ext4_dir_is_empty(uint32_t dir_ino, const ext4_inode_t *dir_inode)
{
    uint64_t dir_size;

    (void)dir_ino;
    if (!dir_inode) return 0;
    dir_size = ext4_inode_size(dir_inode);
    for (uint32_t lblock = 0U; lblock < (uint32_t)(dir_size / g_ext4.block_size); lblock++) {
        uint64_t pblock = 0ULL;
        const uint8_t *block;
        int rc = ext4_map_lblock(dir_inode, lblock, &pblock);
        if (rc < 0) return 0;
        if (pblock == 0ULL)
            continue;
        if (ext4_cache_get(pblock, &block) < 0)
            return 0;

        for (uint32_t off = 0U; off + 8U <= g_ext4.block_size; ) {
            const ext4_dir_entry_2_t *de = (const ext4_dir_entry_2_t *)(const void *)(block + off);
            uint16_t rec_len = de->rec_len;

            if (rec_len < 8U || (rec_len & 3U) != 0U || off + rec_len > g_ext4.block_size)
                return 0;
            if (ext4_dir_is_tail(de))
                break;
            if (de->inode != 0U &&
                !(de->name_len == 1U && de->name[0] == '.') &&
                !(de->name_len == 2U && de->name[0] == '.' && de->name[1] == '.'))
                return 0;
            off += rec_len;
        }
    }

    return 1;
}

static int ext4_dir_update_dotdot(uint32_t dir_ino, ext4_inode_t *dir_inode, uint32_t new_parent_ino)
{
    uint64_t pblock = 0ULL;
    uint8_t *block;

    if (!dir_inode) return -EFAULT;
    if (ext4_map_lblock(dir_inode, 0U, &pblock) < 0 || pblock == 0ULL)
        return -EIO;
    if (ext4_cache_get_mut(pblock, &block) < 0)
        return -EIO;

    {
        ext4_dir_entry_2_t *dot = (ext4_dir_entry_2_t *)(void *)block;
        ext4_dir_entry_2_t *dotdot = (ext4_dir_entry_2_t *)(void *)(block + dot->rec_len);
        dotdot->inode = new_parent_ino;
    }

    dir_inode->i_mtime = (uint32_t)(timer_now_ms() / 1000ULL);
    dir_inode->i_ctime = dir_inode->i_mtime;
    return ext4_dir_update_checksum(dir_ino, dir_inode, pblock);
}

static int ext4_split_parent_child(const char *relpath, char *parent, size_t parent_cap,
                                   char *name, size_t name_cap)
{
    size_t len;
    size_t end;
    size_t slash = (size_t)-1;

    if (!relpath || !parent || !name || parent_cap == 0U || name_cap == 0U)
        return -EINVAL;

    len = e_strlen(relpath);
    while (len > 0U && relpath[len - 1U] == '/')
        len--;
    if (len == 0U)
        return -EINVAL;

    while (slash + 1U < len) {
        size_t i = slash + 1U;
        slash = (size_t)-1;
        for (; i < len; i++) {
            if (relpath[i] == '/')
                slash = i;
        }
        break;
    }

    if (slash == (size_t)-1 || slash == 0U) {
        e_strlcpy(parent, "/", parent_cap);
        end = (relpath[0] == '/') ? 1U : 0U;
    } else {
        if (slash >= parent_cap)
            return -ENAMETOOLONG;
        e_memcpy(parent, relpath, slash);
        parent[slash] = '\0';
        end = slash + 1U;
    }

    if (len - end >= name_cap)
        return -ENAMETOOLONG;
    e_memcpy(name, relpath + end, len - end);
    name[len - end] = '\0';

    if (name[0] == '\0' ||
        e_streq(name, ".") || e_streq(name, ".."))
        return -EINVAL;
    return 0;
}

static uint32_t ext4_mode_from_ftype(uint8_t ft)
{
    switch (ft) {
    case EXT4_FT_REG_FILE: return S_IFREG;
    case EXT4_FT_DIR:      return S_IFDIR;
    case EXT4_FT_CHRDEV:   return S_IFCHR;
    case EXT4_FT_SYMLINK:  return S_IFLNK;
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

static int ext4_lookup_parent_path(const char *relpath,
                                   uint32_t *parent_ino,
                                   ext4_inode_t *parent_inode,
                                   char *name,
                                   size_t name_cap)
{
    char parent[EXT4_NAME_MAX + 8];
    int  rc;

    if (!parent_ino || !parent_inode || !name)
        return -EFAULT;
    rc = ext4_split_parent_child(relpath, parent, sizeof(parent), name, name_cap);
    if (rc < 0) return rc;
    return ext4_lookup_path(parent, parent_ino, parent_inode);
}

static void ext4_init_new_inode(ext4_inode_t *inode, uint16_t mode, uint32_t ino)
{
    uint32_t now = (uint32_t)(timer_now_ms() / 1000ULL);

    e_memset(inode, 0U, sizeof(*inode));
    inode->i_mode = mode;
    inode->i_atime = now;
    inode->i_ctime = now;
    inode->i_mtime = now;
    inode->i_links_count = ext4_inode_file_type(inode) == EXT4_FT_DIR ? 2U : 1U;
    inode->i_flags = EXT4_INODE_FLAG_EXTENTS;
    inode->i_generation = (uint32_t)(0xA5A50000U ^ ino ^ now);
    inode->i_extra_isize = g_ext4.sb.s_want_extra_isize;
    ext4_inode_init_extent_root(inode);
    ext4_inode_refresh_blocks_field(inode);
}

static int ext4_create_file(const char *relpath, uint32_t flags,
                            const vfs_mount_t *mount, vfs_file_t *out)
{
    ext4_inode_t parent_inode;
    ext4_inode_t new_inode;
    uint32_t     parent_ino;
    uint32_t     new_ino;
    char         name[EXT4_NAME_MAX + 1];
    int          rc;

    rc = ext4_lookup_parent_path(relpath, &parent_ino, &parent_inode, name, sizeof(name));
    if (rc < 0) return rc;
    if (!ext4_inode_is_dir(&parent_inode))
        return -ENOTDIR;
    if (ext4_lookup_child(parent_ino, &parent_inode, name, e_strlen(name), &new_ino, &new_inode) == 0)
        return -EEXIST;

    rc = ext4_alloc_inode_preferred((parent_ino - 1U) / g_ext4.inodes_per_group, 0, &new_ino);
    if (rc < 0) return rc;

    ext4_init_new_inode(&new_inode, EXT4_DEF_FILE_MODE, new_ino);
    if (ext4_mark_inode_dirty(new_ino, &new_inode) < 0) {
        (void)ext4_free_inode_bitmap(new_ino, 0);
        return -EIO;
    }

    rc = ext4_dir_add_entry(parent_ino, &parent_inode, new_ino, EXT4_FT_REG_FILE,
                            name, (uint16_t)e_strlen(name));
    if (rc < 0) {
        (void)ext4_free_inode_bitmap(new_ino, 0);
        return rc;
    }
    if (ext4_mark_inode_dirty(parent_ino, &parent_inode) < 0)
        return -EIO;

    if (out) {
        out->mount = mount;
        out->node_id = new_ino;
        out->flags = flags;
        out->pos = ((flags & O_APPEND) != 0U) ? ext4_inode_size(&new_inode) : 0ULL;
        out->size_hint = ext4_inode_size(&new_inode);
        out->dir_index = 0U;
        out->cookie = (uintptr_t)new_inode.i_mode;
    }
    return 0;
}

static int ext4_create_dir(const char *relpath, uint32_t mode)
{
    ext4_inode_t parent_inode;
    ext4_inode_t new_inode;
    uint32_t     parent_ino;
    uint32_t     new_ino;
    uint64_t     pblock = 0ULL;
    char         name[EXT4_NAME_MAX + 1];
    int          rc;

    rc = ext4_lookup_parent_path(relpath, &parent_ino, &parent_inode, name, sizeof(name));
    if (rc < 0) return rc;
    if (!ext4_inode_is_dir(&parent_inode))
        return -ENOTDIR;
    if (ext4_lookup_child(parent_ino, &parent_inode, name, e_strlen(name), &new_ino, &new_inode) == 0)
        return -EEXIST;

    rc = ext4_alloc_inode_preferred((parent_ino - 1U) / g_ext4.inodes_per_group, 1, &new_ino);
    if (rc < 0) return rc;

    ext4_init_new_inode(&new_inode, (uint16_t)(S_IFDIR | (mode & 0777U)), new_ino);
    if ((new_inode.i_mode & 0777U) == 0U)
        new_inode.i_mode = EXT4_DEF_DIR_MODE;

    rc = ext4_ensure_lblock_allocated(new_ino, &new_inode, 0U, &pblock);
    if (rc < 0) {
        (void)ext4_free_inode_bitmap(new_ino, 1);
        return rc;
    }
    rc = ext4_dir_init_new_dir_block(new_ino, &new_inode, parent_ino, pblock);
    if (rc < 0) {
        (void)ext4_free_block(pblock);
        (void)ext4_free_inode_bitmap(new_ino, 1);
        return rc;
    }
    if (ext4_mark_inode_dirty(new_ino, &new_inode) < 0)
        return -EIO;

    rc = ext4_dir_add_entry(parent_ino, &parent_inode, new_ino, EXT4_FT_DIR,
                            name, (uint16_t)e_strlen(name));
    if (rc < 0)
        return rc;
    parent_inode.i_links_count++;
    parent_inode.i_mtime = (uint32_t)(timer_now_ms() / 1000ULL);
    parent_inode.i_ctime = parent_inode.i_mtime;
    return ext4_mark_inode_dirty(parent_ino, &parent_inode);
}

static int ext4_create_symlink(const char *relpath, const char *target)
{
    ext4_inode_t parent_inode;
    ext4_inode_t new_inode;
    uint32_t     parent_ino;
    uint32_t     new_ino;
    size_t       target_len;
    char         name[EXT4_NAME_MAX + 1];
    int          rc;

    if (!target)
        return -EFAULT;

    target_len = e_strlen(target);
    if (target_len == 0U || target_len > sizeof(new_inode.i_block))
        return -ENAMETOOLONG;

    rc = ext4_lookup_parent_path(relpath, &parent_ino, &parent_inode, name, sizeof(name));
    if (rc < 0) return rc;
    if (!ext4_inode_is_dir(&parent_inode))
        return -ENOTDIR;
    if (ext4_lookup_child(parent_ino, &parent_inode, name, e_strlen(name), &new_ino, &new_inode) == 0)
        return -EEXIST;

    rc = ext4_alloc_inode_preferred((parent_ino - 1U) / g_ext4.inodes_per_group, 0, &new_ino);
    if (rc < 0) return rc;

    ext4_init_new_inode(&new_inode, (uint16_t)(S_IFLNK | 0777U), new_ino);
    new_inode.i_flags = 0U;
    new_inode.i_links_count = 1U;
    e_memset(new_inode.i_block, 0U, sizeof(new_inode.i_block));
    e_memcpy(new_inode.i_block, target, target_len);
    ext4_inode_set_size(&new_inode, target_len);
    new_inode.i_blocks_lo = 0U;
    new_inode.i_blocks_high = 0U;

    if (ext4_mark_inode_dirty(new_ino, &new_inode) < 0) {
        (void)ext4_free_inode_bitmap(new_ino, 0);
        return -EIO;
    }

    rc = ext4_dir_add_entry(parent_ino, &parent_inode, new_ino, EXT4_FT_SYMLINK,
                            name, (uint16_t)e_strlen(name));
    if (rc < 0) {
        (void)ext4_free_inode_bitmap(new_ino, 0);
        return rc;
    }
    if (ext4_mark_inode_dirty(parent_ino, &parent_inode) < 0)
        return -EIO;
    return 0;
}

static int ext4_remove_path(const char *relpath)
{
    ext4_inode_t parent_inode;
    ext4_inode_t child_inode;
    uint32_t     parent_ino;
    uint32_t     child_ino;
    char         name[EXT4_NAME_MAX + 1];
    int          rc;

    rc = ext4_lookup_parent_path(relpath, &parent_ino, &parent_inode, name, sizeof(name));
    if (rc < 0) return rc;
    rc = ext4_lookup_child(parent_ino, &parent_inode, name, e_strlen(name), &child_ino, &child_inode);
    if (rc < 0) return rc;

    if (ext4_inode_is_dir(&child_inode) && !ext4_dir_is_empty(child_ino, &child_inode))
        return -ENOTEMPTY;

    rc = ext4_dir_remove_entry(parent_ino, &parent_inode, name, (uint16_t)e_strlen(name), NULL, NULL);
    if (rc < 0) return rc;

    if (ext4_inode_is_dir(&child_inode) && parent_inode.i_links_count > 0U)
        parent_inode.i_links_count--;
    if (ext4_mark_inode_dirty(parent_ino, &parent_inode) < 0)
        return -EIO;

    child_inode.i_dtime = (uint32_t)(timer_now_ms() / 1000ULL);
    child_inode.i_links_count = 0U;
    child_inode.i_mtime = child_inode.i_dtime;
    child_inode.i_ctime = child_inode.i_dtime;
    if (!ext4_inode_is_fast_symlink(&child_inode)) {
        rc = ext4_free_inode_all_blocks(&child_inode);
        if (rc < 0) return rc;
    }
    if (ext4_mark_inode_dirty(child_ino, &child_inode) < 0)
        return -EIO;
    return ext4_free_inode_bitmap(child_ino, ext4_inode_is_dir(&child_inode));
}

static int ext4_readlink_path(const char *relpath, char *out, size_t cap)
{
    ext4_inode_t inode;
    uint32_t     ino;
    size_t       len;
    int          rc;

    if (!out || cap < 2U)
        return -EFAULT;

    rc = ext4_lookup_path(relpath, &ino, &inode);
    if (rc < 0)
        return rc;
    (void)ino;
    if (!ext4_inode_is_symlink(&inode))
        return -EINVAL;

    len = (size_t)ext4_inode_size(&inode);
    if (len > sizeof(inode.i_block) || len + 1U > cap)
        return -ENAMETOOLONG;

    e_memcpy(out, inode.i_block, len);
    out[len] = '\0';
    return 0;
}

static int ext4_rename_path(const char *old_relpath, const char *new_relpath)
{
    ext4_inode_t old_parent_inode;
    ext4_inode_t new_parent_inode;
    ext4_inode_t child_inode;
    ext4_inode_t scratch_inode;
    uint32_t     old_parent_ino;
    uint32_t     new_parent_ino;
    uint32_t     child_ino;
    uint32_t     existing_ino;
    char         old_name[EXT4_NAME_MAX + 1];
    char         new_name[EXT4_NAME_MAX + 1];
    int          rc;

    if (e_streq(old_relpath, new_relpath))
        return 0;

    rc = ext4_lookup_parent_path(old_relpath, &old_parent_ino, &old_parent_inode,
                                 old_name, sizeof(old_name));
    if (rc < 0) return rc;
    rc = ext4_lookup_parent_path(new_relpath, &new_parent_ino, &new_parent_inode,
                                 new_name, sizeof(new_name));
    if (rc < 0) return rc;
    rc = ext4_lookup_child(old_parent_ino, &old_parent_inode, old_name, e_strlen(old_name),
                           &child_ino, &child_inode);
    if (rc < 0) return rc;
    if (ext4_inode_is_dir(&child_inode)) {
        size_t old_len = e_strlen(old_relpath);
        size_t new_len = e_strlen(new_relpath);
        if (new_len > old_len && new_relpath[old_len] == '/') {
            size_t i;
            for (i = 0U; i < old_len; i++) {
                if (old_relpath[i] != new_relpath[i])
                    break;
            }
            if (i == old_len)
                return -EINVAL;
        }
    }
    if (ext4_lookup_child(new_parent_ino, &new_parent_inode, new_name, e_strlen(new_name),
                          &existing_ino, &scratch_inode) == 0)
        return -EEXIST;

    rc = ext4_dir_add_entry(new_parent_ino, &new_parent_inode, child_ino,
                            ext4_inode_file_type(&child_inode), new_name,
                            (uint16_t)e_strlen(new_name));
    if (rc < 0) return rc;
    if (ext4_mark_inode_dirty(new_parent_ino, &new_parent_inode) < 0)
        return -EIO;

    rc = ext4_dir_remove_entry(old_parent_ino, &old_parent_inode, old_name,
                               (uint16_t)e_strlen(old_name), NULL, NULL);
    if (rc < 0)
        return rc;

    if (ext4_inode_is_dir(&child_inode) && old_parent_ino != new_parent_ino) {
        rc = ext4_dir_update_dotdot(child_ino, &child_inode, new_parent_ino);
        if (rc < 0) return rc;
        if (old_parent_inode.i_links_count > 0U)
            old_parent_inode.i_links_count--;
        new_parent_inode.i_links_count++;
        if (ext4_mark_inode_dirty(new_parent_ino, &new_parent_inode) < 0)
            return -EIO;
    }

    child_inode.i_ctime = (uint32_t)(timer_now_ms() / 1000ULL);
    if (ext4_mark_inode_dirty(child_ino, &child_inode) < 0)
        return -EIO;
    return ext4_mark_inode_dirty(old_parent_ino, &old_parent_inode);
}

static int ext4_journal_locate_blocks(uint32_t ino, ext4_inode_t *inode)
{
    uint64_t need_size = (uint64_t)EXT4_JOURNAL_FILE_BLOCKS * g_ext4.block_size;
    int      changed = 0;

    if (!inode) return -EFAULT;
    if ((inode->i_mode & S_IFMT) != S_IFREG)
        return -EINVAL;

    if (ext4_inode_size(inode) < need_size) {
        int rc = ext4_truncate_inode(ino, inode, need_size);
        if (rc < 0) return rc;
        changed = 1;
    }

    for (uint32_t lblock = 0U; lblock < EXT4_JOURNAL_FILE_BLOCKS; lblock++) {
        uint64_t pblock = 0ULL;
        int      rc = ext4_map_lblock(inode, lblock, &pblock);
        if (rc < 0) return rc;
        if (pblock == 0ULL) {
            rc = ext4_ensure_lblock_allocated(ino, inode, lblock, &pblock);
            if (rc < 0) return rc;
            changed = 1;
        }
        if (pblock == 0ULL)
            return -EIO;
        g_ext4.journal.phys_blocks[lblock] = pblock;
    }

    if (changed) {
        if (ext4_mark_inode_dirty(ino, inode) < 0)
            return -EIO;
        if (ext4_sync_raw() < 0)
            return -EIO;
    }

    g_ext4.journal.ino = ino;
    return 0;
}

static int ext4_journal_recover_if_needed(void)
{
    uint8_t               header_block[EXT4_MAX_BLOCK_SIZE];
    ext4_journal_header_t hdr;
    uint8_t               tmp[EXT4_MAX_BLOCK_SIZE];

    if (!g_ext4.journal.ready)
        return 0;
    if (ext4_raw_block_read(g_ext4.journal.phys_blocks[0], header_block) < 0)
        return -EIO;

    e_memcpy(&hdr, header_block, sizeof(hdr));
    if (hdr.magic == 0U && hdr.state == EXT4_JOURNAL_STATE_CLEAN)
        return 0;
    if (hdr.magic != EXT4_JOURNAL_MAGIC ||
        hdr.version != EXT4_JOURNAL_VERSION ||
        hdr.block_size != g_ext4.block_size ||
        hdr.entry_count > EXT4_JOURNAL_MAX_TX_BLOCKS) {
        ext4_set_error("journal custom corrotto");
        return -EIO;
    }

    e_memcpy(tmp, header_block, g_ext4.block_size);
    *(uint32_t *)(void *)(tmp + EXT4_JOURNAL_HEADER_CRC_OFF) = 0U;
    if (ext4_crc32c_raw(0xFFFFFFFFU, tmp, g_ext4.block_size) != hdr.header_crc) {
        ext4_set_error("checksum journal custom non valida");
        return -EIO;
    }

    if (hdr.state != EXT4_JOURNAL_STATE_PENDING || hdr.entry_count == 0U)
        return 0;

    for (uint32_t i = 0U; i < hdr.entry_count; i++) {
        const ext4_journal_entry_t *entry;
        uint8_t                     data_block[EXT4_MAX_BLOCK_SIZE];

        entry = (const ext4_journal_entry_t *)(const void *)
                (header_block + sizeof(ext4_journal_header_t) +
                 i * sizeof(ext4_journal_entry_t));

        if (ext4_raw_block_read(g_ext4.journal.phys_blocks[i + 1U], data_block) < 0)
            return -EIO;
        if (ext4_journal_block_crc(data_block) != entry->data_crc) {
            ext4_set_error("dati journal custom corrotti");
            return -EIO;
        }
        if (ext4_raw_block_write(entry->target_block, data_block) < 0)
            return -EIO;
    }

    if (blk_flush_sync() != BLK_OK)
        return -EIO;
    if (ext4_journal_clear_disk() < 0)
        return -EIO;

    ext4_cache_invalidate();
    uart_puts("[EXT4] recovery: journal custom replay OK\n");
    return 0;
}

static int ext4_journal_init(void)
{
    ext4_inode_t inode;
    uint32_t     ino;
    int          rc;

    rc = ext4_lookup_path(EXT4_INTERNAL_JOURNAL_NAME, &ino, &inode);
    if (rc == -ENOENT) {
        rc = ext4_create_file(EXT4_INTERNAL_JOURNAL_NAME,
                              O_WRONLY | O_CREAT, NULL, NULL);
        if (rc < 0)
            return rc;
        rc = ext4_lookup_path(EXT4_INTERNAL_JOURNAL_NAME, &ino, &inode);
    }
    if (rc < 0)
        return rc;

    rc = ext4_journal_locate_blocks(ino, &inode);
    if (rc < 0)
        return rc;

    g_ext4.journal.ready = true;
    return ext4_journal_recover_if_needed();
}

static int ext4_open_vfs(const vfs_mount_t *mount, const char *relpath,
                         uint32_t flags, vfs_file_t *out)
{
    ext4_inode_t inode;
    uint32_t     ino;
    uint32_t     access = (flags & 0x3U);
    int          lock_rc;
    int          rc;

    if (!out) return -EFAULT;
    if (!g_ext4.mounted) return -EIO;
    if (ext4_internal_journal_path(relpath))
        return -ENOENT;

    lock_rc = ext4_open_lock_enter();
    if (lock_rc < 0)
        return lock_rc;

    rc = ext4_lookup_path(relpath, &ino, &inode);
    if (rc < 0) {
        if (rc == -ENOENT && (flags & O_CREAT) != 0U)
            rc = ext4_create_file(relpath, flags, mount, out);
        goto out;
    }
    if (ext4_inode_is_dir(&inode) &&
        (access == O_WRONLY || access == O_RDWR)) {
        rc = -EISDIR;
        goto out;
    }
    if ((flags & O_TRUNC) != 0U) {
        if (!(access == O_WRONLY || access == O_RDWR)) {
            rc = -EINVAL;
            goto out;
        }
        if (ext4_inode_is_dir(&inode)) {
            rc = -EISDIR;
            goto out;
        }
        rc = ext4_truncate_inode(ino, &inode, 0ULL);
        if (rc < 0)
            goto out;
    }

    out->mount     = mount;
    out->node_id   = ino;
    out->flags     = flags;
    out->pos       = ((flags & O_APPEND) != 0U) ? ext4_inode_size(&inode) : 0ULL;
    out->size_hint = ext4_inode_size(&inode);
    out->dir_index = 0U;
    out->cookie    = (uintptr_t)inode.i_mode;
    rc = 0;

out:
    ext4_open_lock_exit(lock_rc);
    return rc;
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
    ext4_inode_t inode;
    uint64_t     pos;
    size_t       written = 0U;
    uint32_t     access;
    int          rc;

    if (!file) return -EBADF;
    if (!buf && count != 0U) return -EFAULT;

    access = (file->flags & 0x3U);
    if (!(access == O_WRONLY || access == O_RDWR))
        return -EBADF;

    rc = ext4_load_inode(file->node_id, &inode);
    if (rc < 0) return rc;
    if (ext4_inode_is_dir(&inode)) return -EISDIR;

    pos = ((file->flags & O_APPEND) != 0U) ? ext4_inode_size(&inode) : file->pos;
    rc = ext4_write_inode_data(file->node_id, &inode, pos, buf, count, &written);
    if (rc < 0) return rc;

    file->pos = pos + written;
    file->size_hint = ext4_inode_size(&inode);
    return (ssize_t)written;
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
        if (file->node_id == EXT4_ROOT_INO &&
            e_streq(out->name, EXT4_INTERNAL_JOURNAL_NAME))
            continue;

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

static int ext4_lstat_vfs(const vfs_mount_t *mount, const char *relpath, stat_t *out)
{
    ext4_inode_t inode;
    uint32_t     ino;
    int          rc;

    (void)mount;
    if (!out) return -EFAULT;
    rc = ext4_lookup_path(relpath, &ino, &inode);
    if (rc < 0) return rc;
    (void)ino;

    out->st_mode = inode.i_mode;
    out->st_blksize = g_ext4.block_size;
    out->st_size = ext4_inode_size(&inode);
    out->st_blocks = ext4_inode_blocks(&inode);
    return 0;
}

static int ext4_close_vfs(vfs_file_t *file)
{
    uint32_t access;

    (void)file;
    if (!file) return -EBADF;

    access = (file->flags & 0x3U);
    if (access == O_WRONLY || access == O_RDWR ||
        (file->flags & (O_TRUNC | O_APPEND)) != 0U)
        return ext4_sync();

    return 0;
}

static int ext4_mkdir_vfs(const vfs_mount_t *mount, const char *relpath, uint32_t mode)
{
    (void)mount;
    if (ext4_internal_journal_path(relpath))
        return -EROFS;
    return ext4_create_dir(relpath, mode);
}

static int ext4_unlink_vfs(const vfs_mount_t *mount, const char *relpath)
{
    (void)mount;
    if (ext4_internal_journal_path(relpath))
        return -EROFS;
    return ext4_remove_path(relpath);
}

static int ext4_symlink_vfs(const vfs_mount_t *mount, const char *target,
                            const char *relpath)
{
    (void)mount;
    if (ext4_internal_journal_path(relpath))
        return -EROFS;
    return ext4_create_symlink(relpath, target);
}

static int ext4_readlink_vfs(const vfs_mount_t *mount, const char *relpath,
                             char *out, size_t cap)
{
    (void)mount;
    return ext4_readlink_path(relpath, out, cap);
}

static int ext4_rename_vfs(const vfs_mount_t *old_mount, const char *old_relpath,
                           const vfs_mount_t *new_mount, const char *new_relpath)
{
    (void)old_mount;
    (void)new_mount;
    if (ext4_internal_journal_path(old_relpath) ||
        ext4_internal_journal_path(new_relpath))
        return -EROFS;
    return ext4_rename_path(old_relpath, new_relpath);
}

static int ext4_fsync_vfs(vfs_file_t *file)
{
    (void)file;
    return ext4_sync();
}

static int ext4_truncate_vfs(const vfs_mount_t *mount, const char *relpath, uint64_t size)
{
    ext4_inode_t inode;
    uint32_t     ino;
    int          rc;

    (void)mount;
    if (ext4_internal_journal_path(relpath))
        return -EROFS;
    rc = ext4_lookup_path(relpath, &ino, &inode);
    if (rc < 0) return rc;
    if (ext4_inode_is_dir(&inode)) return -EISDIR;
    return ext4_truncate_inode(ino, &inode, size);
}

static int ext4_sync_vfs(const vfs_mount_t *mount)
{
    (void)mount;
    return ext4_sync();
}

static const vfs_ops_t ext4_ops = {
    .open    = ext4_open_vfs,
    .read    = ext4_read_vfs,
    .write   = ext4_write_vfs,
    .readdir = ext4_readdir_vfs,
    .stat    = ext4_stat_vfs,
    .lstat   = ext4_lstat_vfs,
    .close   = ext4_close_vfs,
    .mkdir   = ext4_mkdir_vfs,
    .unlink  = ext4_unlink_vfs,
    .symlink = ext4_symlink_vfs,
    .readlink = ext4_readlink_vfs,
    .rename  = ext4_rename_vfs,
    .fsync   = ext4_fsync_vfs,
    .truncate = ext4_truncate_vfs,
    .sync    = ext4_sync_vfs,
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

    if (g_ext4_open_lock == KMON_INVALID && current_task) {
        rc = kmon_create_current(PRIO_HIGH, KMON_HOARE, &g_ext4_open_lock);
        if (rc < 0)
            return rc;
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
    e_memcpy(g_ext4.sb_raw, sb_raw, sizeof(sb_raw));
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
    g_ext4.metadata_csum =
        (sb.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0U;
    g_ext4.has_dir_index =
        (sb.s_feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX) != 0U;
    g_ext4.has_journal =
        (sb.s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) != 0U;
    if ((sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) != 0U)
        g_ext4.checksum_seed = *(uint32_t *)(void *)(sb_raw + EXT4_SUPER_CSUM_SEED_OFF);
    else
        g_ext4.checksum_seed = ext4_crc32c_raw(0xFFFFFFFFU, sb.s_uuid, sizeof(sb.s_uuid));
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
        ext4_set_error("journal replay richiesto: mount rw-core rifiutato");
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
    rc = ext4_journal_init();
    if (rc < 0) {
        g_ext4.mounted = false;
        if (g_ext4.status[0] == '\0')
            ext4_set_error("journal custom init fallita");
        return rc;
    }

    status_reset();
    status_append("mount rw-full OK: label=");
    status_append(g_ext4.label);
    status_append(" block=");
    status_append_u32(g_ext4.block_size);
    status_append(" groups=");
    status_append_u32(g_ext4.group_count);
    status_append(" blocks=");
    status_append_u64(g_ext4.block_count);
    status_append(" journal=bounded");
    ext4_log_status("[EXT4] ");
    return 0;
}

void ext4_unmount(void)
{
    if (g_ext4.mounted)
        (void)ext4_sync();
    ext4_reset_state();
}

int ext4_is_mounted(void)
{
    return g_ext4.mounted ? 1 : 0;
}

int ext4_has_dirty(void)
{
    return ext4_has_dirty_internal();
}

int ext4_service_writeback(uint64_t min_age_ms)
{
    uint64_t now;

    if (!g_ext4.mounted)
        return 0;
    if (!ext4_has_dirty_internal())
        return 0;

    now = timer_now_ms();
    if (min_age_ms != 0ULL && now - g_ext4.last_dirty_ms < min_age_ms)
        return 0;

    return ext4_sync();
}

static int ext4_selftest_force_recovery_cycle(void)
{
    int rc;

    if (!g_ext4.mounted || !g_ext4.journal.ready || !ext4_has_dirty_internal())
        return -EINVAL;

    rc = ext4_txn_build(&g_ext4_txn);
    if (rc < 0) return rc;
    rc = ext4_journal_write_txn(&g_ext4_txn);
    if (rc < 0) return rc;

    ext4_reset_state();
    return ext4_mount();
}

int ext4_selftest_recovery(void)
{
    static const char test_path[] = "SELFTEST.DIR/JRECOVERY.TXT";
    static const char expect[] = "journal-replay-ok";
    ext4_inode_t      inode;
    uint32_t          ino;
    char              buf[32];
    size_t            written = 0U;
    int               rc;

    rc = ext4_lookup_path(test_path, &ino, &inode);
    if (rc == -ENOENT) {
        rc = ext4_create_file(test_path, O_WRONLY | O_CREAT | O_TRUNC, NULL, NULL);
        if (rc < 0) return rc;
        if (ext4_sync() < 0) return -EIO;
        rc = ext4_lookup_path(test_path, &ino, &inode);
    }
    if (rc < 0) return rc;

    rc = ext4_truncate_inode(ino, &inode, 0ULL);
    if (rc < 0) return rc;
    rc = ext4_write_inode_data(ino, &inode, 0ULL, expect, sizeof(expect) - 1U, &written);
    if (rc < 0) return rc;
    if (written != sizeof(expect) - 1U)
        return -EIO;

    rc = ext4_selftest_force_recovery_cycle();
    if (rc < 0) return rc;

    rc = ext4_lookup_path(test_path, &ino, &inode);
    if (rc < 0) return rc;
    e_memset(buf, 0U, sizeof(buf));
    rc = ext4_read_inode_data(&inode, 0ULL, buf, sizeof(expect) - 1U);
    if (rc < 0) return rc;
    if (!e_nameeq(buf, sizeof(expect) - 1U, expect, sizeof(expect) - 1U))
        return -EIO;

    return 0;
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
