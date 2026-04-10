#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

__thread int  tls_seed = 0x12345678;
__thread int  tls_zero;
__thread char tls_tag[8] = "seed";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static int             g_ready_count;
static int             g_release;

static void demo_sleep_brief(void)
{
    static const struct timespec step = { 0L, 1000000L };
    (void)nanosleep(&step, NULL);
}

static void *demo_tls_worker(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;

    if (tls_seed != 0x12345678)
        return (void *)(100UL + idx);
    if (tls_zero != 0)
        return (void *)(110UL + idx);
    if (strcmp(tls_tag, "seed") != 0)
        return (void *)(120UL + idx);

    errno = (int)(40U + idx);
    tls_seed += (int)idx;
    tls_zero = (int)(idx * 10U);
    tls_tag[0] = (char)('A' + (int)idx);
    tls_tag[1] = '\0';

    if (pthread_mutex_lock(&g_lock) != 0)
        return (void *)(130UL + idx);
    g_ready_count++;
    if (pthread_cond_signal(&g_cond) != 0) {
        (void)pthread_mutex_unlock(&g_lock);
        return (void *)(140UL + idx);
    }
    while (!g_release) {
        if (pthread_cond_wait(&g_cond, &g_lock) != 0) {
            (void)pthread_mutex_unlock(&g_lock);
            return (void *)(150UL + idx);
        }
    }
    if (pthread_mutex_unlock(&g_lock) != 0)
        return (void *)(160UL + idx);

    for (int i = 0; i < 8; i++)
        demo_sleep_brief();

    if (errno != (int)(40U + idx))
        return (void *)(170UL + idx);
    if (tls_seed != 0x12345678 + (int)idx)
        return (void *)(180UL + idx);
    if (tls_zero != (int)(idx * 10U))
        return (void *)(190UL + idx);
    if (tls_tag[0] != (char)('A' + (int)idx) || tls_tag[1] != '\0')
        return (void *)(200UL + idx);

    return (void *)(0x9000UL + idx);
}

int main(void)
{
    static const char out[] =
        "tls-template-ok\n"
        "thread-isolation-ok\n"
        "errno-tls-ok\n"
        "join-ok\n";
    pthread_t worker1;
    pthread_t worker2;
    void     *ret1 = 0;
    void     *ret2 = 0;
    int       fd;

    if (tls_seed != 0x12345678)
        return 1;
    if (tls_zero != 0)
        return 2;
    if (strcmp(tls_tag, "seed") != 0)
        return 3;

    errno = EIO;
    tls_seed = 0x55667788;
    tls_zero = 77;
    strcpy(tls_tag, "main");

    if (pthread_create(&worker1, NULL, demo_tls_worker, (void *)1UL) != 0)
        return 4;
    if (pthread_create(&worker2, NULL, demo_tls_worker, (void *)2UL) != 0)
        return 5;

    if (pthread_mutex_lock(&g_lock) != 0)
        return 6;
    while (g_ready_count < 2) {
        if (pthread_cond_wait(&g_cond, &g_lock) != 0) {
            (void)pthread_mutex_unlock(&g_lock);
            return 7;
        }
    }

    if (errno != EIO || tls_seed != 0x55667788 || tls_zero != 77 ||
        strcmp(tls_tag, "main") != 0) {
        (void)pthread_mutex_unlock(&g_lock);
        return 8;
    }

    g_release = 1;
    if (pthread_cond_broadcast(&g_cond) != 0) {
        (void)pthread_mutex_unlock(&g_lock);
        return 9;
    }
    if (pthread_mutex_unlock(&g_lock) != 0)
        return 10;

    if (pthread_join(worker1, &ret1) != 0)
        return 11;
    if (pthread_join(worker2, &ret2) != 0)
        return 12;
    if (ret1 != (void *)0x9001UL || ret2 != (void *)0x9002UL)
        return 13;

    if (errno != EIO || tls_seed != 0x55667788 || tls_zero != 77 ||
        strcmp(tls_tag, "main") != 0)
        return 14;

    fd = open("/data/TLSMTDEMO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 15;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 16;
    }
    if (close(fd) != 0)
        return 17;
    return 0;
}
