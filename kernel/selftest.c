/*
 * EnlilOS Microkernel - Self-Test Suite
 *
 * Valida i sottosistemi recenti senza dipendere da input manuale:
 *   - bootstrap VFS / devfs
 *   - ext4 rw-core su virtio-blk
 *   - GPU memory manager / renderer 2D / present path
 */

#include "selftest.h"
#include "blk.h"
#include "ext4.h"
#include "gpu.h"
#include "syscall.h"
#include "uart.h"
#include "vfs.h"

extern int gpu_selftest_run(void);

typedef int (*selftest_case_fn)(void);

typedef struct {
    const char       *name;
    selftest_case_fn  fn;
} selftest_case_t;

static uint32_t st_strlen(const char *s)
{
    uint32_t n = 0U;
    if (!s) return 0U;
    while (s[n] != '\0')
        n++;
    return n;
}

static int st_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b);
}

static int st_contains(const char *haystack, const char *needle)
{
    uint32_t needle_len;

    if (!haystack || !needle) return 0;
    needle_len = st_strlen(needle);
    if (needle_len == 0U) return 1;

    for (uint32_t i = 0U; haystack[i] != '\0'; i++) {
        uint32_t j = 0U;
        while (needle[j] != '\0' && haystack[i + j] == needle[j])
            j++;
        if (j == needle_len)
            return 1;
    }
    return 0;
}

static void st_put_u32(uint32_t v)
{
    char buf[10];
    uint32_t len = 0U;

    if (v == 0U) {
        uart_putc('0');
        return;
    }

    while (v != 0U && len < (uint32_t)sizeof(buf)) {
        buf[len++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (len > 0U)
        uart_putc(buf[--len]);
}

static void st_log_fail(const char *case_name, const char *detail)
{
    uart_puts("[SELFTEST] FAIL ");
    uart_puts(case_name);
    uart_puts(": ");
    uart_puts(detail);
    uart_puts("\n");
}

#define ST_CHECK(case_name, cond, detail) \
    do { \
        if (!(cond)) { \
            st_log_fail((case_name), (detail)); \
            return -1; \
        } \
    } while (0)

static int selftest_case_rootfs(void)
{
    static const char case_name[] = "vfs-rootfs";
    vfs_file_t dir;
    vfs_file_t file;
    vfs_dirent_t ent;
    char buf[192];
    ssize_t n;
    int rc;

    rc = vfs_open("/", O_RDONLY, &dir);
    ST_CHECK(case_name, rc == 0, "open / fallita");

    rc = vfs_readdir(&dir, &ent);
    ST_CHECK(case_name, rc == 0, "readdir / #0 fallita");
    ST_CHECK(case_name, st_streq(ent.name, "README.TXT"),
             "rootfs: primo entry inatteso");

    rc = vfs_readdir(&dir, &ent);
    ST_CHECK(case_name, rc == 0, "readdir / #1 fallita");
    ST_CHECK(case_name, st_streq(ent.name, "BOOT.TXT"),
             "rootfs: secondo entry inatteso");
    (void)vfs_close(&dir);

    rc = vfs_open("/BOOT.TXT", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open /BOOT.TXT fallita");

    n = vfs_read(&file, buf, sizeof(buf) - 1U);
    ST_CHECK(case_name, n > 0, "read /BOOT.TXT vuota");
    buf[n] = '\0';
    (void)vfs_close(&file);

    ST_CHECK(case_name, st_contains(buf, "Mount profile bootstrap:"),
             "BOOT.TXT non contiene il profilo mount");
    ST_CHECK(case_name, st_contains(buf, "/dev     -> devfs"),
             "BOOT.TXT non contiene /dev");
    return 0;
}

static int selftest_case_devfs(void)
{
    static const char case_name[] = "vfs-devfs";
    vfs_file_t dir;
    vfs_file_t file;
    vfs_dirent_t ent;
    stat_t st;
    char null_data[4] = { 't', 'e', 's', 't' };
    ssize_t n;
    int rc;

    rc = vfs_open("/dev", O_RDONLY, &dir);
    ST_CHECK(case_name, rc == 0, "open /dev fallita");

    rc = vfs_readdir(&dir, &ent);
    ST_CHECK(case_name, rc == 0, "readdir /dev #0 fallita");
    ST_CHECK(case_name, st_streq(ent.name, "console"),
             "devfs: primo entry inatteso");
    rc = vfs_readdir(&dir, &ent);
    ST_CHECK(case_name, rc == 0, "readdir /dev #1 fallita");
    ST_CHECK(case_name, st_streq(ent.name, "tty"),
             "devfs: secondo entry inatteso");
    (void)vfs_close(&dir);

    rc = vfs_open("/dev/stdout", O_WRONLY, &file);
    ST_CHECK(case_name, rc == 0, "open /dev/stdout fallita");
    rc = vfs_stat(&file, &st);
    ST_CHECK(case_name, rc == 0, "stat /dev/stdout fallita");
    ST_CHECK(case_name, (st.st_mode & S_IFMT) == S_IFCHR,
             "stdout non e' un char device");
    (void)vfs_close(&file);

    rc = vfs_open("/dev/null", O_WRONLY, &file);
    ST_CHECK(case_name, rc == 0, "open /dev/null fallita");
    n = vfs_write(&file, null_data, sizeof(null_data));
    ST_CHECK(case_name, n == (ssize_t)sizeof(null_data),
             "write /dev/null non ha consumato tutti i byte");
    (void)vfs_close(&file);
    return 0;
}

static int selftest_case_ext4(void)
{
    static const char case_name[] = "ext4-core";
    vfs_file_t file;
    stat_t st;
    char buf[64];
    ssize_t n;
    const char *status = ext4_status();
    int rc;

    ST_CHECK(case_name, blk_is_ready() == 1, "virtio-blk non pronto");
    ST_CHECK(case_name, blk_sector_count() > 0ULL, "capacita' blk nulla");
    ST_CHECK(case_name, ext4_is_mounted() == 1, "ext4 non montato");
    ST_CHECK(case_name, status && st_contains(status, "mount rw-full OK"),
             "status ext4 non segnala mount riuscito");

    rc = vfs_open("/data/lost+found", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open /data/lost+found fallita");
    rc = vfs_stat(&file, &st);
    ST_CHECK(case_name, rc == 0, "stat /data/lost+found fallita");
    ST_CHECK(case_name, (st.st_mode & S_IFMT) == S_IFDIR,
             "/data/lost+found non e' una directory");
    (void)vfs_close(&file);

    rc = vfs_open("/sysroot/lost+found", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open /sysroot/lost+found fallita");
    rc = vfs_stat(&file, &st);
    ST_CHECK(case_name, rc == 0, "stat /sysroot/lost+found fallita");
    ST_CHECK(case_name, (st.st_mode & S_IFMT) == S_IFDIR,
             "/sysroot/lost+found non e' una directory");
    (void)vfs_close(&file);

    rc = vfs_open("/data/lost+found", O_WRONLY, &file);
    ST_CHECK(case_name, rc == -EISDIR,
             "open O_WRONLY su directory non ha dato EISDIR");

    rc = vfs_truncate("/data/lost+found", 0ULL);
    ST_CHECK(case_name, rc == -EISDIR,
             "truncate directory non ha dato EISDIR");

    rc = vfs_sync();
    ST_CHECK(case_name, rc == 0, "vfs_sync fallita");

    (void)vfs_unlink("/data/SELFTEST.DIR/RENAMED.TXT");
    (void)vfs_unlink("/data/SELFTEST.DIR/HELLO.TXT");
    (void)vfs_unlink("/data/SELFTEST.DIR");

    rc = vfs_mkdir("/data/SELFTEST.DIR", 0755U);
    ST_CHECK(case_name, rc == 0, "mkdir /data/SELFTEST.DIR fallita");

    rc = vfs_open("/data/SELFTEST.DIR/HELLO.TXT", O_WRONLY | O_CREAT | O_TRUNC, &file);
    ST_CHECK(case_name, rc == 0, "open create HELLO.TXT fallita");
    n = vfs_write(&file, "hello", 5U);
    ST_CHECK(case_name, n == 5, "write iniziale HELLO.TXT fallita");
    rc = vfs_fsync(&file);
    ST_CHECK(case_name, rc == 0, "fsync HELLO.TXT fallita");
    (void)vfs_close(&file);

    rc = vfs_open("/data/SELFTEST.DIR/HELLO.TXT", O_WRONLY | O_APPEND, &file);
    ST_CHECK(case_name, rc == 0, "open append HELLO.TXT fallita");
    n = vfs_write(&file, " world", 6U);
    ST_CHECK(case_name, n == 6, "append HELLO.TXT fallita");
    (void)vfs_close(&file);

    rc = vfs_open("/data/SELFTEST.DIR/HELLO.TXT", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open read HELLO.TXT fallita");
    n = vfs_read(&file, buf, sizeof(buf) - 1U);
    ST_CHECK(case_name, n == 11, "read HELLO.TXT inattesa");
    buf[n] = '\0';
    ST_CHECK(case_name, st_streq(buf, "hello world"),
             "contenuto HELLO.TXT inatteso");
    (void)vfs_close(&file);

    rc = vfs_rename("/data/SELFTEST.DIR/HELLO.TXT", "/data/SELFTEST.DIR/RENAMED.TXT");
    ST_CHECK(case_name, rc == 0, "rename HELLO->RENAMED fallita");

    rc = vfs_open("/data/SELFTEST.DIR/HELLO.TXT", O_RDONLY, &file);
    ST_CHECK(case_name, rc == -ENOENT, "old path dopo rename ancora visibile");

    rc = vfs_unlink("/data/SELFTEST.DIR/RENAMED.TXT");
    ST_CHECK(case_name, rc == 0, "unlink RENAMED.TXT fallita");
    rc = vfs_unlink("/data/SELFTEST.DIR");
    ST_CHECK(case_name, rc == 0, "unlink SELFTEST.DIR fallita");

    rc = vfs_sync();
    ST_CHECK(case_name, rc == 0, "vfs_sync finale fallita");
    return 0;
}

int selftest_run_all(void)
{
    static const selftest_case_t cases[] = {
        { "vfs-rootfs", selftest_case_rootfs },
        { "vfs-devfs",  selftest_case_devfs  },
        { "ext4-core",  selftest_case_ext4   },
        { "gpu-stack",  gpu_selftest_run     },
    };
    uint32_t total = 0U;
    uint32_t passed = 0U;
    uint32_t failed = 0U;

    uart_puts("\n[SELFTEST] ===== EnlilOS self-test suite =====\n");

    for (uint32_t i = 0U; i < (uint32_t)(sizeof(cases) / sizeof(cases[0])); i++) {
        int rc;

        total++;
        uart_puts("[SELFTEST] RUN  ");
        uart_puts(cases[i].name);
        uart_puts("\n");

        rc = cases[i].fn();
        if (rc == 0) {
            passed++;
            uart_puts("[SELFTEST] PASS ");
            uart_puts(cases[i].name);
            uart_puts("\n");
        } else {
            failed++;
        }
    }

    uart_puts("[SELFTEST] SUMMARY total=");
    st_put_u32(total);
    uart_puts(" pass=");
    st_put_u32(passed);
    uart_puts(" fail=");
    st_put_u32(failed);
    uart_puts("\n");

    return (failed == 0U) ? 0 : -1;
}
