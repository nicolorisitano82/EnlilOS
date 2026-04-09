#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static volatile int    g_ready;
static volatile int    g_release;
static volatile int    g_detached_done;
static volatile int    g_sigusr1_seen;

static void demo_sleep_brief(void)
{
    static const struct timespec step = { 0L, 1000000L };
    (void)nanosleep(&step, NULL);
}

static void demo_sigusr1(int sig)
{
    (void)sig;
    g_sigusr1_seen = 1;
}

static void *demo_worker(void *arg)
{
    sigset_t set;
    char     cwd[64];

    if (arg != (void *)0xCAFEUL)
        return (void *)1UL;
    if (!pthread_equal(pthread_self(), pthread_self()))
        return (void *)2UL;

    (void)sigemptyset(&set);
    (void)sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0)
        return (void *)3UL;

    if (chdir("/data") != 0)
        return (void *)4UL;
    if (!getcwd(cwd, sizeof(cwd)))
        return (void *)5UL;
    if (strcmp(cwd, "/data") != 0)
        return (void *)6UL;

    if (pthread_mutex_lock(&g_lock) != 0)
        return (void *)7UL;
    g_ready = 1;
    if (pthread_cond_signal(&g_cond) != 0) {
        (void)pthread_mutex_unlock(&g_lock);
        return (void *)8UL;
    }
    while (!g_release) {
        if (pthread_cond_wait(&g_cond, &g_lock) != 0) {
            (void)pthread_mutex_unlock(&g_lock);
            return (void *)9UL;
        }
    }
    if (pthread_mutex_unlock(&g_lock) != 0)
        return (void *)10UL;

    for (int i = 0; i < 1000 && !g_sigusr1_seen; i++)
        demo_sleep_brief();
    if (!g_sigusr1_seen)
        return (void *)11UL;
    return (void *)0x1234UL;
}

static void *demo_detached(void *arg)
{
    (void)arg;
    g_detached_done = 1;
    return NULL;
}

int main(void)
{
    static const char out[] =
        "mutex-cond-ok\n"
        "join-ok\n"
        "detach-ok\n"
        "signal-ok\n"
        "cwd-share-ok\n";
    struct sigaction sa;
    sigset_t         set;
    pthread_t        worker;
    pthread_t        detached;
    pthread_attr_t   attr;
    void            *retval = NULL;
    char             cwd[64];
    int              fd;

    sa.sa_handler = demo_sigusr1;
    sa.sa_mask = 0ULL;
    sa.sa_flags = 0U;
    sa.__pad = 0U;
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
        return 1;

    (void)sigemptyset(&set);
    (void)sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
        return 2;

    if (pthread_create(&worker, NULL, demo_worker, (void *)0xCAFEUL) != 0)
        return 3;

    if (pthread_mutex_lock(&g_lock) != 0)
        return 4;
    while (!g_ready) {
        if (pthread_cond_wait(&g_cond, &g_lock) != 0) {
            (void)pthread_mutex_unlock(&g_lock);
            return 5;
        }
    }
    g_release = 1;
    if (pthread_cond_broadcast(&g_cond) != 0) {
        (void)pthread_mutex_unlock(&g_lock);
        return 6;
    }
    if (pthread_mutex_unlock(&g_lock) != 0)
        return 7;

    if (!getcwd(cwd, sizeof(cwd)))
        return 8;
    if (strcmp(cwd, "/data") != 0)
        return 9;

    if (pthread_kill(worker, SIGUSR1) != 0)
        return 10;
    if (pthread_join(worker, &retval) != 0)
        return 11;
    if (retval != (void *)0x1234UL)
        return 12;

    if (pthread_attr_init(&attr) != 0)
        return 13;
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
        return 14;
    if (pthread_create(&detached, &attr, demo_detached, NULL) != 0)
        return 15;
    (void)detached;
    (void)pthread_attr_destroy(&attr);

    for (int i = 0; i < 1000 && !g_detached_done; i++)
        demo_sleep_brief();
    if (!g_detached_done)
        return 16;

    fd = open("/data/PTHREADDEMO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 17;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 18;
    }
    if (close(fd) != 0)
        return 19;
    return 0;
}
