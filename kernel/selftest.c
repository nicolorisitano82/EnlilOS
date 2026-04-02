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
#include "elf_loader.h"
#include "ext4.h"
#include "gpu.h"
#include "kdebug.h"
#include "kmon.h"
#include "ksem.h"
#include "microkernel.h"
#include "mmu.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer.h"
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
    stat_t st;
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
    ST_CHECK(case_name, st_contains(buf, "initrd cpio"),
             "BOOT.TXT non conferma l'initrd CPIO");

    rc = vfs_open("/INIT.ELF", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open /INIT.ELF fallita");
    rc = vfs_stat(&file, &st);
    ST_CHECK(case_name, rc == 0, "stat /INIT.ELF fallita");
    ST_CHECK(case_name, (st.st_mode & S_IFMT) == S_IFREG,
             "/INIT.ELF non e' un file regolare");
    ST_CHECK(case_name, st.st_size > 0ULL, "/INIT.ELF ha size zero");
    (void)vfs_close(&file);

    rc = vfs_open("/NSH.ELF", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open /NSH.ELF fallita");
    rc = vfs_stat(&file, &st);
    ST_CHECK(case_name, rc == 0, "stat /NSH.ELF fallita");
    ST_CHECK(case_name, (st.st_mode & S_IFMT) == S_IFREG,
             "/NSH.ELF non e' un file regolare");
    ST_CHECK(case_name, st.st_size > 0ULL, "/NSH.ELF ha size zero");
    (void)vfs_close(&file);
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

    (void)vfs_unlink("/data/SELFTEST.DIR/JRECOVERY.TXT");
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

    rc = ext4_selftest_recovery();
    ST_CHECK(case_name, rc == 0, "journal recovery selftest fallito");

    rc = vfs_unlink("/data/SELFTEST.DIR/JRECOVERY.TXT");
    ST_CHECK(case_name, rc == 0, "unlink JRECOVERY.TXT fallita");

    rc = vfs_unlink("/data/SELFTEST.DIR");
    ST_CHECK(case_name, rc == 0, "unlink SELFTEST.DIR fallita");

    rc = vfs_sync();
    ST_CHECK(case_name, rc == 0, "vfs_sync finale fallita");
    return 0;
}

static int selftest_case_elf(void)
{
    static const char case_name[] = "elf-loader";
    uint32_t pid = 0U;
    uint64_t deadline;
    sched_tcb_t *task;
    int rc;

    rc = elf64_spawn_path("/DEMO.ELF", "/DEMO.ELF", PRIO_KERNEL, &pid);
    ST_CHECK(case_name, rc == 0, elf64_last_error());
    ST_CHECK(case_name, pid != 0U, "pid demo ELF nullo");

    deadline = timer_now_ms() + 2000ULL;
    do {
        task = sched_task_find(pid);
        ST_CHECK(case_name, task != NULL, "task ELF non trovata");
        if (task->state == TCB_STATE_ZOMBIE)
            return 0;
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, 0, "timeout attesa task ELF");
    return -1;
}

static int selftest_case_execve(void)
{
    static const char case_name[] = "execve";
    uint32_t pid = 0U;
    uint64_t deadline;
    sched_tcb_t *task;
    int rc;

    rc = elf64_spawn_path("/EXEC1.ELF", "/EXEC1.ELF", PRIO_KERNEL, &pid);
    ST_CHECK(case_name, rc == 0, elf64_last_error());
    ST_CHECK(case_name, pid != 0U, "pid execve demo nullo");

    deadline = timer_now_ms() + 2000ULL;
    do {
        task = sched_task_find(pid);
        ST_CHECK(case_name, task != NULL, "task execve non trovata");
        if (task->state == TCB_STATE_ZOMBIE)
            return 0;
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, 0, "timeout attesa execve");
    return -1;
}

static int selftest_case_dynelf(void)
{
    static const char case_name[] = "elf-dynamic";
    uint32_t pid = 0U;
    uint64_t deadline;
    sched_tcb_t *task;
    int rc;

    rc = elf64_spawn_path("/DYNDEMO.ELF", "/DYNDEMO.ELF", PRIO_KERNEL, &pid);
    ST_CHECK(case_name, rc == 0, elf64_last_error());
    ST_CHECK(case_name, pid != 0U, "pid dynamic demo nullo");

    deadline = timer_now_ms() + 2000ULL;
    do {
        task = sched_task_find(pid);
        ST_CHECK(case_name, task != NULL, "task dynamic ELF non trovata");
        if (task->state == TCB_STATE_ZOMBIE)
            return 0;
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, 0, "timeout attesa dynamic ELF");
    return -1;
}

static int selftest_case_fork(void)
{
    static const char case_name[] = "fork-cow";
    static const char expected[] =
        "child global=7 local=99\n"
        "parent global=1 local=11\n";
    uint32_t    pid = 0U;
    uint64_t    deadline;
    sched_tcb_t *task;
    vfs_file_t  file;
    char        buf[96];
    ssize_t     n;
    int         rc;

    rc = vfs_unlink("/data/FORK.TXT");
    ST_CHECK(case_name, rc == 0 || rc == -ENOENT, "cleanup FORK.TXT fallita");

    rc = elf64_spawn_path("/FORKDEMO.ELF", "/FORKDEMO.ELF", PRIO_KERNEL, &pid);
    ST_CHECK(case_name, rc == 0, elf64_last_error());
    ST_CHECK(case_name, pid != 0U, "pid fork demo nullo");

    deadline = timer_now_ms() + 2000ULL;
    do {
        task = sched_task_find(pid);
        ST_CHECK(case_name, task != NULL, "task fork demo non trovata");
        if (task->state == TCB_STATE_ZOMBIE)
            break;
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, task && task->state == TCB_STATE_ZOMBIE,
             "timeout attesa fork demo");

    rc = vfs_open("/data/FORK.TXT", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open FORK.TXT fallita");
    n = vfs_read(&file, buf, sizeof(buf) - 1U);
    ST_CHECK(case_name, n > 0, "read FORK.TXT fallita");
    buf[(n < (ssize_t)(sizeof(buf) - 1U)) ? (size_t)n : (sizeof(buf) - 1U)] = '\0';
    ST_CHECK(case_name, st_streq(buf, expected), "contenuto FORK.TXT inatteso");
    (void)vfs_close(&file);

    rc = vfs_unlink("/data/FORK.TXT");
    ST_CHECK(case_name, rc == 0, "unlink FORK.TXT fallita");
    return 0;
}

static int selftest_case_signal(void)
{
    static const char case_name[] = "signal-core";
    static const char expected[] =
        "child-term\n"
        "parent-chld\n";
    uint32_t    pid = 0U;
    uint64_t    deadline;
    sched_tcb_t *task;
    vfs_file_t  file;
    char        buf[96];
    ssize_t     n;
    int         rc;

    rc = vfs_unlink("/data/SIGNAL.TXT");
    ST_CHECK(case_name, rc == 0 || rc == -ENOENT, "cleanup SIGNAL.TXT fallita");
    rc = vfs_unlink("/data/SIGREADY.TXT");
    ST_CHECK(case_name, rc == 0 || rc == -ENOENT, "cleanup SIGREADY.TXT fallita");

    rc = elf64_spawn_path("/SIGDEMO.ELF", "/SIGDEMO.ELF", PRIO_KERNEL, &pid);
    ST_CHECK(case_name, rc == 0, elf64_last_error());
    ST_CHECK(case_name, pid != 0U, "pid signal demo nullo");

    deadline = timer_now_ms() + 3000ULL;
    do {
        task = sched_task_find(pid);
        ST_CHECK(case_name, task != NULL, "task signal demo non trovata");
        if (task->state == TCB_STATE_ZOMBIE)
            break;
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, task && task->state == TCB_STATE_ZOMBIE,
             "timeout attesa signal demo");

    rc = vfs_open("/data/SIGNAL.TXT", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open SIGNAL.TXT fallita");
    n = vfs_read(&file, buf, sizeof(buf) - 1U);
    ST_CHECK(case_name, n > 0, "read SIGNAL.TXT fallita");
    buf[(n < (ssize_t)(sizeof(buf) - 1U)) ? (size_t)n : (sizeof(buf) - 1U)] = '\0';
    ST_CHECK(case_name, st_streq(buf, expected), "contenuto SIGNAL.TXT inatteso");
    (void)vfs_close(&file);

    rc = vfs_unlink("/data/SIGNAL.TXT");
    ST_CHECK(case_name, rc == 0, "unlink SIGNAL.TXT fallita");
    rc = vfs_unlink("/data/SIGREADY.TXT");
    ST_CHECK(case_name, rc == 0, "unlink SIGREADY.TXT fallita");
    return 0;
}

static int selftest_case_mreact(void)
{
    static const char case_name[] = "mreact-core";
    uint32_t          pid = 0U;
    uint64_t          deadline;
    sched_tcb_t      *task;
    mm_space_t       *space;
    volatile uint32_t *cell;
    vfs_file_t        file;
    char              buf[16];
    ssize_t           n;
    int               rc;

    rc = vfs_unlink("/data/MREACT.TXT");
    ST_CHECK(case_name, rc == 0 || rc == -ENOENT, "cleanup MREACT.TXT fallita");
    rc = vfs_unlink("/data/MREACT.READY");
    ST_CHECK(case_name, rc == 0 || rc == -ENOENT, "cleanup MREACT.READY fallita");

    rc = elf64_spawn_path("/MREACTDEMO.ELF", "/MREACTDEMO.ELF", PRIO_KERNEL, &pid);
    ST_CHECK(case_name, rc == 0, elf64_last_error());
    ST_CHECK(case_name, pid != 0U, "pid mreact demo nullo");

    deadline = timer_now_ms() + 2000ULL;
    do {
        task = sched_task_find(pid);
        ST_CHECK(case_name, task != NULL, "task mreact demo non trovata");

        rc = vfs_open("/data/MREACT.READY", O_RDONLY, &file);
        if (rc == 0) {
            (void)vfs_close(&file);
            break;
        }
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, rc == 0, "timeout attesa MREACT.READY");

    space = sched_task_space(task);
    ST_CHECK(case_name, space != NULL, "mm_space mreact nulla");
    rc = mmu_space_prepare_write(space, MMU_USER_BASE, sizeof(uint32_t));
    ST_CHECK(case_name, rc == 0, "prepare_write target mreact fallita");

    cell = (volatile uint32_t *)mmu_space_resolve_ptr(space, MMU_USER_BASE,
                                                      sizeof(uint32_t));
    ST_CHECK(case_name, cell != NULL, "resolve_ptr target mreact fallita");
    *cell = 42U;

    deadline = timer_now_ms() + 2000ULL;
    do {
        task = sched_task_find(pid);
        ST_CHECK(case_name, task != NULL, "task mreact demo non trovata");
        if (task->state == TCB_STATE_ZOMBIE)
            break;
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, task->state == TCB_STATE_ZOMBIE,
             "timeout attesa mreact demo");

    rc = vfs_open("/data/MREACT.TXT", O_RDONLY, &file);
    ST_CHECK(case_name, rc == 0, "open MREACT.TXT fallita");
    n = vfs_read(&file, buf, sizeof(buf) - 1U);
    ST_CHECK(case_name, n > 0, "read MREACT.TXT fallita");
    buf[(n < (ssize_t)(sizeof(buf) - 1U)) ? (size_t)n : (sizeof(buf) - 1U)] = '\0';
    ST_CHECK(case_name, st_streq(buf, "ok\n"), "contenuto MREACT.TXT inatteso");
    (void)vfs_close(&file);

    rc = vfs_unlink("/data/MREACT.TXT");
    ST_CHECK(case_name, rc == 0, "unlink MREACT.TXT fallita");
    rc = vfs_unlink("/data/MREACT.READY");
    ST_CHECK(case_name, rc == 0, "unlink MREACT.READY fallita");
    return 0;
}

static volatile uint32_t ipc_test_port_id;
static volatile uint32_t ipc_test_server_waiting;
static volatile uint32_t ipc_test_server_ok;
static volatile uint32_t ipc_test_client_ok;
static volatile uint32_t ipc_test_hog_release;
static volatile uint32_t ipc_test_server_effprio;
static volatile uint64_t ipc_test_reply_value;

static volatile uint32_t ksem_test_holder_ready;
static volatile uint32_t ksem_test_holder_release;
static volatile uint32_t ksem_test_waiter_ok;
static volatile int32_t  ksem_test_waiter_rc;
static volatile uint32_t ksem_test_hog_release;

static volatile kmon_t   kmon_test_handle;
static volatile uint32_t kmon_test_holder_ready;
static volatile uint32_t kmon_test_holder_release;
static volatile uint32_t kmon_test_holder_effprio;
static volatile uint32_t kmon_test_enter_ok;
static volatile uint32_t kmon_test_cond_waiting;
static volatile uint32_t kmon_test_cond_value;
static volatile uint32_t kmon_test_cond_ack;
static volatile uint32_t kmon_test_signal_ok;
static volatile int32_t  kmon_test_wait_rc;
static volatile uint32_t kmon_test_timeout_ok;
static volatile uint32_t kmon_test_signal_seen_cond;

static void selftest_ksem_holder_task(void)
{
    static const char sem_name[] = "/selftest-ksem-rt";
    ksem_t handle = KSEM_INVALID;

    if (ksem_open_current(sem_name, 0U, &handle) == 0) {
        if (ksem_wait_current(handle) == 0) {
            ksem_test_holder_ready = 1U;
            while (!ksem_test_holder_release)
                sched_yield();
            (void)ksem_post_current(handle);
        }
        (void)ksem_close_current(handle);
    }
}

static void selftest_ksem_waiter_task(void)
{
    static const char sem_name[] = "/selftest-ksem-rt";
    ksem_t handle = KSEM_INVALID;
    int    rc = -ENOENT;

    if (ksem_open_current(sem_name, 0U, &handle) == 0) {
        rc = ksem_timedwait_current(handle, 500000000ULL);
        if (rc == 0) {
            ksem_test_waiter_ok = 1U;
            (void)ksem_post_current(handle);
        }
        (void)ksem_close_current(handle);
    }

    ksem_test_waiter_rc = rc;
}

static void selftest_ksem_hog_task(void)
{
    while (!ksem_test_hog_release)
        __asm__ volatile("" ::: "memory");
}

static int selftest_case_ksem(void)
{
    static const char case_name[] = "ksem-core";
    static const char sem_name[] = "/selftest-ksem-rt";
    sched_tcb_t *holder = NULL;
    sched_tcb_t *waiter = NULL;
    sched_tcb_t *hog = NULL;
    ksem_t       main_handle = KSEM_INVALID;
    ksem_t       anon_handle = KSEM_INVALID;
    int32_t      value = -1;
    uint64_t     deadline;
    int          rc;

    (void)ksem_unlink_current(sem_name);
    rc = ksem_create_current(sem_name, 1U, KSEM_SHARED | KSEM_RT, &main_handle);
    ST_CHECK(case_name, rc == 0, "create named ksem fallita");

    rc = ksem_getvalue_current(main_handle, &value);
    ST_CHECK(case_name, rc == 0 && value == 1, "getvalue iniziale inatteso");

    rc = ksem_trywait_current(main_handle);
    ST_CHECK(case_name, rc == 0, "trywait iniziale fallita");
    rc = ksem_getvalue_current(main_handle, &value);
    ST_CHECK(case_name, rc == 0 && value == 0, "value dopo trywait inatteso");
    rc = ksem_post_current(main_handle);
    ST_CHECK(case_name, rc == 0, "post iniziale fallita");

    ksem_test_holder_ready = 0U;
    ksem_test_holder_release = 0U;
    ksem_test_waiter_ok = 0U;
    ksem_test_waiter_rc = 0;
    ksem_test_hog_release = 0U;

    holder = sched_task_create("ksem-holder", selftest_ksem_holder_task, PRIO_HIGH);
    ST_CHECK(case_name, holder != NULL, "creazione holder task fallita");

    deadline = timer_now_ms() + 1000ULL;
    while (!ksem_test_holder_ready && timer_now_ms() < deadline)
        sched_yield();
    ST_CHECK(case_name, ksem_test_holder_ready != 0U,
             "holder non ha acquisito il semaforo");
    holder->priority = PRIO_LOW;

    hog = sched_task_create("ksem-hog", selftest_ksem_hog_task, PRIO_NORMAL);
    ST_CHECK(case_name, hog != NULL, "creazione hog task fallita");
    waiter = sched_task_create("ksem-waiter", selftest_ksem_waiter_task, PRIO_KERNEL);
    ST_CHECK(case_name, waiter != NULL, "creazione waiter task fallita");

    deadline = timer_now_ms() + 1000ULL;
    while (sched_task_effective_priority(holder) != PRIO_KERNEL &&
           timer_now_ms() < deadline)
        sched_yield();

    if (sched_task_effective_priority(holder) != PRIO_KERNEL) {
        ksem_test_holder_release = 1U;
        ksem_test_hog_release = 1U;
        ST_CHECK(case_name, 0, "priority inheritance non applicata");
    }

    ksem_test_holder_release = 1U;

    deadline = timer_now_ms() + 1000ULL;
    while (!ksem_test_waiter_ok && timer_now_ms() < deadline)
        sched_yield();

    ksem_test_hog_release = 1U;
    ST_CHECK(case_name, ksem_test_waiter_ok != 0U,
             "waiter non ha acquisito il semaforo");
    ST_CHECK(case_name, ksem_test_waiter_rc == 0,
             "timedwait waiter non ha ritornato 0");

    deadline = timer_now_ms() + 1000ULL;
    while (((holder && sched_task_find(holder->pid) &&
             sched_task_find(holder->pid)->state != TCB_STATE_ZOMBIE) ||
            (waiter && sched_task_find(waiter->pid) &&
             sched_task_find(waiter->pid)->state != TCB_STATE_ZOMBIE) ||
            (hog && sched_task_find(hog->pid) &&
             sched_task_find(hog->pid)->state != TCB_STATE_ZOMBIE)) &&
           timer_now_ms() < deadline)
        sched_yield();

    rc = ksem_getvalue_current(main_handle, &value);
    ST_CHECK(case_name, rc == 0 && value == 1,
             "value finale del semaforo RT inatteso");

    rc = ksem_unlink_current(sem_name);
    ST_CHECK(case_name, rc == 0, "unlink named ksem fallita");
    rc = ksem_open_current(sem_name, 0U, &anon_handle);
    ST_CHECK(case_name, rc == -ENOENT, "open dopo unlink non ha dato ENOENT");

    rc = ksem_close_current(main_handle);
    ST_CHECK(case_name, rc == 0, "close named ksem fallita");

    rc = ksem_anon_current(0U, 0U, &anon_handle);
    ST_CHECK(case_name, rc == 0, "create anon ksem fallita");
    rc = ksem_trywait_current(anon_handle);
    ST_CHECK(case_name, rc == -EAGAIN, "trywait anon non ha dato EAGAIN");
    rc = ksem_timedwait_current(anon_handle, 2000000ULL);
    ST_CHECK(case_name, rc == -ETIMEDOUT,
             "timedwait anon non ha dato ETIMEDOUT");
    rc = ksem_post_current(anon_handle);
    ST_CHECK(case_name, rc == 0, "post anon fallita");
    rc = ksem_wait_current(anon_handle);
    ST_CHECK(case_name, rc == 0, "wait anon dopo post fallita");
    rc = ksem_close_current(anon_handle);
    ST_CHECK(case_name, rc == 0, "close anon ksem fallita");
    return 0;
}

static void selftest_kmon_holder_task(void)
{
    if (kmon_enter_current((kmon_t)kmon_test_handle) == 0) {
        uart_puts("[SELFTEST] kmon-holder acquired\n");
        kmon_test_holder_effprio = sched_task_effective_priority(current_task);
        kmon_test_holder_ready = 1U;
        while (!kmon_test_holder_release)
            sched_yield();
        (void)kmon_exit_current((kmon_t)kmon_test_handle);
        uart_puts("[SELFTEST] kmon-holder exited\n");
    }
    sched_task_exit();
}

static void selftest_kmon_enter_task(void)
{
    if (kmon_enter_current((kmon_t)kmon_test_handle) == 0) {
        uart_puts("[SELFTEST] kmon-enter acquired\n");
        kmon_test_enter_ok = 1U;
        (void)kmon_exit_current((kmon_t)kmon_test_handle);
        uart_puts("[SELFTEST] kmon-enter exited\n");
    }
    sched_task_exit();
}

static void selftest_kmon_waiter_task(void)
{
    int rc = -EIO;

    if (kmon_enter_current((kmon_t)kmon_test_handle) == 0) {
        kmon_test_cond_waiting = 1U;
        if (kmon_test_cond_value == 0U)
            rc = kmon_wait_current((kmon_t)kmon_test_handle, 0U, 500000000ULL);
        else
            rc = 0;

        kmon_test_wait_rc = rc;
        if (rc == 0 && kmon_test_cond_value == 1U)
            kmon_test_cond_ack = 1U;
        (void)kmon_exit_current((kmon_t)kmon_test_handle);
    } else {
        kmon_test_wait_rc = -EIO;
    }
    sched_task_exit();
}

static void selftest_kmon_signaler_task(void)
{
    while (!kmon_test_cond_waiting)
        sched_yield();

    if (kmon_enter_current((kmon_t)kmon_test_handle) == 0) {
        kmon_test_cond_value = 1U;
        kmon_test_signal_seen_cond = kmon_test_cond_value;
        if (kmon_signal_current((kmon_t)kmon_test_handle, 0U) == 0) {
            kmon_test_signal_ok = 1U;
        }
        (void)kmon_exit_current((kmon_t)kmon_test_handle);
    }
    sched_task_exit();
}

static int selftest_case_kmon(void)
{
    static const char case_name[] = "kmon-core";
    sched_tcb_t *holder = NULL;
    sched_tcb_t *enterer = NULL;
    sched_tcb_t *waiter = NULL;
    sched_tcb_t *signaler = NULL;
    kmon_t       handle = KMON_INVALID;
    uint64_t     deadline;
    int          rc;

    rc = kmon_create_current(PRIO_HIGH, KMON_HOARE | KMON_RT, &handle);
    ST_CHECK(case_name, rc == 0, "create monitor fallita");
    uart_puts("[SELFTEST] kmon phase create\n");
    kmon_test_handle = handle;
    kmon_test_holder_ready = 0U;
    kmon_test_holder_release = 0U;
    kmon_test_holder_effprio = 0xFFU;
    kmon_test_enter_ok = 0U;
    kmon_test_cond_waiting = 0U;
    kmon_test_cond_value = 0U;
    kmon_test_cond_ack = 0U;
    kmon_test_signal_ok = 0U;
    kmon_test_wait_rc = -EIO;
    kmon_test_timeout_ok = 0U;
    kmon_test_signal_seen_cond = 0U;

    holder = sched_task_create("kmon-holder", selftest_kmon_holder_task, PRIO_HIGH);
    ST_CHECK(case_name, holder != NULL, "creazione holder monitor fallita");

    deadline = timer_now_ms() + 1000ULL;
    while (!kmon_test_holder_ready && timer_now_ms() < deadline)
        sched_yield();
    ST_CHECK(case_name, kmon_test_holder_ready != 0U,
             "holder monitor non ha acquisito il lock");
    ST_CHECK(case_name, kmon_test_holder_effprio == PRIO_HIGH,
             "priority ceiling non applicato");
    uart_puts("[SELFTEST] kmon phase holder-ready\n");

    holder->priority = PRIO_LOW;
    enterer = sched_task_create("kmon-enter", selftest_kmon_enter_task, PRIO_KERNEL);
    ST_CHECK(case_name, enterer != NULL, "creazione enter waiter fallita");

    deadline = timer_now_ms() + 1000ULL;
    while (sched_task_effective_priority(holder) != PRIO_KERNEL &&
           timer_now_ms() < deadline)
        sched_yield();
    ST_CHECK(case_name, sched_task_effective_priority(holder) == PRIO_KERNEL,
             "PI sul monitor lock non applicata");
    uart_puts("[SELFTEST] kmon phase pi-ok\n");

    kmon_test_holder_release = 1U;
    deadline = timer_now_ms() + 1000ULL;
    while (!kmon_test_enter_ok && timer_now_ms() < deadline)
        sched_yield();
    ST_CHECK(case_name, kmon_test_enter_ok != 0U,
             "enter waiter non ha acquisito il monitor");
    uart_puts("[SELFTEST] kmon phase enter-ok\n");

    deadline = timer_now_ms() + 1000ULL;
    while (((holder && sched_task_find(holder->pid) &&
             sched_task_find(holder->pid)->state != TCB_STATE_ZOMBIE) ||
            (enterer && sched_task_find(enterer->pid) &&
             sched_task_find(enterer->pid)->state != TCB_STATE_ZOMBIE)) &&
           timer_now_ms() < deadline)
        sched_yield();

    waiter = sched_task_create("kmon-waiter", selftest_kmon_waiter_task, PRIO_HIGH);
    ST_CHECK(case_name, waiter != NULL, "creazione cond waiter fallita");
    signaler = sched_task_create("kmon-signal", selftest_kmon_signaler_task, PRIO_HIGH);
    ST_CHECK(case_name, signaler != NULL, "creazione signaler fallita");

    deadline = timer_now_ms() + 1500ULL;
    while ((!kmon_test_signal_ok || kmon_test_wait_rc != 0) &&
           timer_now_ms() < deadline)
        sched_yield();
    ST_CHECK(case_name, kmon_test_wait_rc == 0,
             "wait/signal monitor non ha completato");
    ST_CHECK(case_name, kmon_test_signal_seen_cond == 1U,
             "signaler non ha pubblicato la condizione");
    ST_CHECK(case_name, kmon_test_cond_ack != 0U,
             "waiter non ha visto la condizione vera");
    ST_CHECK(case_name, kmon_test_signal_ok != 0U,
             "signal monitor non ha completato");

    deadline = timer_now_ms() + 1000ULL;
    while (((waiter && sched_task_find(waiter->pid) &&
             sched_task_find(waiter->pid)->state != TCB_STATE_ZOMBIE) ||
            (signaler && sched_task_find(signaler->pid) &&
             sched_task_find(signaler->pid)->state != TCB_STATE_ZOMBIE)) &&
           timer_now_ms() < deadline)
        sched_yield();

    rc = kmon_enter_current(handle);
    ST_CHECK(case_name, rc == 0, "enter monitor per timedwait fallita");
    rc = kmon_wait_current(handle, 1U, 2000000ULL);
    if (rc == -ETIMEDOUT)
        kmon_test_timeout_ok = 1U;
    ST_CHECK(case_name, rc == -ETIMEDOUT,
             "timedwait monitor non ha dato ETIMEDOUT");
    rc = kmon_exit_current(handle);
    ST_CHECK(case_name, rc == 0, "exit monitor dopo timedwait fallita");

    rc = kmon_destroy_current(handle);
    ST_CHECK(case_name, rc == 0, "destroy monitor fallita");
    return 0;
}

static void selftest_ipc_server_task(void)
{
    ipc_message_t req;
    uint64_t      reply = 0xBEEFFACE11223344ULL;

    ipc_test_server_waiting = 1U;
    if (mk_ipc_wait((uint32_t)ipc_test_port_id, &req) == 0 &&
        req.msg_type == IPC_MSG_PING &&
        req.msg_len == sizeof(uint64_t) &&
        (req.flags & IPC_MSG_FLAG_INLINE) != 0U &&
        req.mr[0] == 0x1122334455667788ULL) {
        ipc_test_server_effprio = sched_task_effective_priority(current_task);
        if (mk_ipc_reply((uint32_t)ipc_test_port_id, IPC_MSG_PONG,
                         &reply, sizeof(reply)) == 0) {
            ipc_test_server_ok = 1U;
        }
    }
}

static void selftest_ipc_hog_task(void)
{
    while (!ipc_test_hog_release)
        __asm__ volatile("" ::: "memory");
}

static void selftest_ipc_client_task(void)
{
    ipc_message_t reply;
    uint64_t      ping = 0x1122334455667788ULL;

    if (mk_ipc_call((uint32_t)ipc_test_port_id, IPC_MSG_PING,
                    &ping, sizeof(ping), &reply) == 0 &&
        reply.msg_type == IPC_MSG_PONG &&
        reply.msg_len == sizeof(uint64_t) &&
        (reply.flags & IPC_MSG_FLAG_INLINE) != 0U &&
        reply.mr[0] == 0xBEEFFACE11223344ULL) {
        ipc_test_reply_value = reply.mr[0];
        ipc_test_client_ok = 1U;
    }

    ipc_test_hog_release = 1U;
}

static int selftest_case_ipc_sync(void)
{
    static const char case_name[] = "ipc-sync";
    sched_tcb_t *server;
    sched_tcb_t *hog;
    sched_tcb_t *client;
    port_stats_t stats;
    uint64_t deadline;
    int rc;

    ipc_test_port_id = 0U;
    ipc_test_server_waiting = 0U;
    ipc_test_server_ok = 0U;
    ipc_test_client_ok = 0U;
    ipc_test_hog_release = 0U;
    ipc_test_server_effprio = 0xFFU;
    ipc_test_reply_value = 0ULL;

    server = sched_task_create("ipc-server", selftest_ipc_server_task, PRIO_KERNEL);
    ST_CHECK(case_name, server != NULL, "creazione task server fallita");

    ipc_test_port_id = mk_port_create(server->pid, "ipc-selftest");
    ST_CHECK(case_name, ipc_test_port_id != 0U, "mk_port_create fallita");
    rc = mk_port_set_budget((uint32_t)ipc_test_port_id, timer_cntfrq() / 125ULL);
    ST_CHECK(case_name, rc == 0, "mk_port_set_budget fallita");

    deadline = timer_now_ms() + 2000ULL;
    do {
        sched_tcb_t *server_now = sched_task_find(server->pid);

        ST_CHECK(case_name, server_now != NULL, "server task scomparso");
        if (ipc_test_server_waiting && server_now->state == TCB_STATE_BLOCKED)
            break;
        sched_yield();
    } while (timer_now_ms() < deadline);

    ST_CHECK(case_name, ipc_test_server_waiting != 0U,
             "server non e' entrato in wait");

    server->priority = PRIO_LOW;

    hog = sched_task_create("ipc-hog", selftest_ipc_hog_task, PRIO_NORMAL);
    ST_CHECK(case_name, hog != NULL, "creazione hog task fallita");
    client = sched_task_create("ipc-client", selftest_ipc_client_task, PRIO_KERNEL);
    ST_CHECK(case_name, client != NULL, "creazione client task fallita");

    deadline = timer_now_ms() + 2000ULL;
    do {
        if (ipc_test_server_ok && ipc_test_client_ok && ipc_test_hog_release) {
            sched_tcb_t *client_now = sched_task_find(client->pid);
            sched_tcb_t *server_now = sched_task_find(server->pid);
            sched_tcb_t *hog_now = sched_task_find(hog->pid);

            ST_CHECK(case_name, client_now != NULL, "client task scomparso");
            ST_CHECK(case_name, server_now != NULL, "server task scomparso");
            ST_CHECK(case_name, hog_now != NULL, "hog task scomparso");
            if (client_now->state == TCB_STATE_ZOMBIE &&
                server_now->state == TCB_STATE_ZOMBIE &&
                hog_now->state == TCB_STATE_ZOMBIE) {
                break;
            }
        }
        sched_yield();
    } while (timer_now_ms() < deadline);

    ipc_test_hog_release = 1U;

    ST_CHECK(case_name, ipc_test_server_ok != 0U, "server non ha risposto");
    ST_CHECK(case_name, ipc_test_client_ok != 0U, "client non ha ricevuto reply");
    ST_CHECK(case_name, ipc_test_reply_value == 0xBEEFFACE11223344ULL,
             "payload reply inatteso");
    ST_CHECK(case_name, ipc_test_server_effprio == PRIO_KERNEL,
             "priority donation non applicata al server");

    rc = mk_port_get_stats((uint32_t)ipc_test_port_id, &stats);
    ST_CHECK(case_name, rc == 0, "mk_port_get_stats fallita");
    ST_CHECK(case_name, stats.total_calls == 1U, "conteggio call inatteso");
    ST_CHECK(case_name, stats.inline_calls == 1U, "small-message inline non rilevato");
    ST_CHECK(case_name, stats.last_call_cycles > 0ULL, "latenza call nulla");
    ST_CHECK(case_name, stats.budget_misses == 0U, "budget di latenza superato");

    rc = mk_port_destroy((uint32_t)ipc_test_port_id);
    ST_CHECK(case_name, rc == 0, "mk_port_destroy fallita");
    return 0;
}

static const selftest_case_t selftest_cases[] = {
    { "vfs-rootfs",  selftest_case_rootfs    },
    { "vfs-devfs",   selftest_case_devfs     },
    { "ext4-core",   selftest_case_ext4      },
    { "elf-loader",  selftest_case_elf       },
    { "execve",      selftest_case_execve    },
    { "elf-dynamic", selftest_case_dynelf    },
    { "fork-cow",    selftest_case_fork      },
    { "signal-core", selftest_case_signal    },
    { "mreact-core", selftest_case_mreact    },
    { "ksem-core",   selftest_case_ksem      },
    { "kmon-core",   selftest_case_kmon      },
    { "ipc-sync",    selftest_case_ipc_sync  },
    { "kdebug-core", kdebug_selftest_run     },
    { "gpu-stack",   gpu_selftest_run        },
};

int selftest_run_named(const char *name)
{
    if (!name || name[0] == '\0')
        return -EINVAL;

    for (uint32_t i = 0U; i < (uint32_t)(sizeof(selftest_cases) / sizeof(selftest_cases[0])); i++) {
        if (!st_streq(selftest_cases[i].name, name))
            continue;

        uart_puts("\n[SELFTEST] ===== EnlilOS self-test =====\n");
        uart_puts("[SELFTEST] RUN  ");
        uart_puts(selftest_cases[i].name);
        uart_puts("\n");

        if (selftest_cases[i].fn() == 0) {
            uart_puts("[SELFTEST] PASS ");
            uart_puts(selftest_cases[i].name);
            uart_puts("\n");
            uart_puts("[SELFTEST] SUMMARY total=1 pass=1 fail=0\n");
            return 0;
        }

        uart_puts("[SELFTEST] SUMMARY total=1 pass=0 fail=1\n");
        return -1;
    }

    uart_puts("[SELFTEST] FAIL selftest: caso non trovato: ");
    uart_puts(name);
    uart_puts("\n");
    return -ENOENT;
}

int selftest_run_all(void)
{
    uint32_t total = 0U;
    uint32_t passed = 0U;
    uint32_t failed = 0U;

    uart_puts("\n[SELFTEST] ===== EnlilOS self-test suite =====\n");

    for (uint32_t i = 0U; i < (uint32_t)(sizeof(selftest_cases) / sizeof(selftest_cases[0])); i++) {
        int rc;

        total++;
        uart_puts("[SELFTEST] RUN  ");
        uart_puts(selftest_cases[i].name);
        uart_puts("\n");

        rc = selftest_cases[i].fn();
        if (rc == 0) {
            passed++;
            uart_puts("[SELFTEST] PASS ");
            uart_puts(selftest_cases[i].name);
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
