#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <unistd.h>

static sem_t        g_unnamed;
static volatile int g_waiter_ready;
static volatile int g_waiter_done;

static void demo_sleep_brief(void)
{
    static const struct timespec step = { 0L, 1000000L };
    (void)nanosleep(&step, NULL);
}

static void *demo_waiter(void *arg)
{
    (void)arg;
    g_waiter_ready = 1;
    if (sem_wait(&g_unnamed) != 0)
        return (void *)1UL;
    g_waiter_done = 1;
    return NULL;
}

int main(void)
{
    static const char out[] =
        "unnamed-ok\n"
        "named-ok\n"
        "timeout-ok\n";
    pthread_t       tid;
    sem_t          *named = SEM_FAILED;
    struct timeval  tv;
    struct timespec past;
    int             value = -1;
    int             fd;

    (void)sem_unlink("/semdemo");

    if (sem_init(&g_unnamed, 0, 0U) != 0)
        return 1;
    if (pthread_create(&tid, NULL, demo_waiter, NULL) != 0)
        return 2;

    for (int i = 0; i < 1000 && !g_waiter_ready; i++)
        demo_sleep_brief();
    if (!g_waiter_ready)
        return 3;
    if (sem_post(&g_unnamed) != 0)
        return 4;
    if (pthread_join(tid, NULL) != 0)
        return 5;
    if (!g_waiter_done)
        return 6;

    named = sem_open("/semdemo", O_CREAT, 0644, 1U);
    if (named == SEM_FAILED)
        return 7;
    if (sem_wait(named) != 0)
        return 8;
    errno = 0;
    if (sem_trywait(named) != -1 || errno != EAGAIN)
        return 9;
    if (sem_post(named) != 0)
        return 10;
    if (sem_getvalue(named, &value) != 0)
        return 11;
    if (value <= 0)
        return 12;
    if (sem_wait(named) != 0)
        return 13;

    for (int i = 0; i < 8; i++) {
        if (gettimeofday(&tv, NULL) != 0)
            return 14;
        if (tv.tv_sec != 0L || tv.tv_usec != 0L)
            break;
        demo_sleep_brief();
    }
    if (tv.tv_usec > 0L) {
        past.tv_sec = tv.tv_sec;
        past.tv_nsec = (tv.tv_usec - 1L) * 1000L;
    } else if (tv.tv_sec > 0L) {
        past.tv_sec = tv.tv_sec - 1L;
        past.tv_nsec = 999999999L;
    } else {
        past.tv_sec = 0L;
        past.tv_nsec = 0L;
    }
    errno = 0;
    if (sem_timedwait(named, &past) != -1 || errno != ETIMEDOUT)
        return 15;

    if (sem_close(named) != 0)
        return 16;
    if (sem_unlink("/semdemo") != 0)
        return 17;
    if (sem_destroy(&g_unnamed) != 0)
        return 18;

    fd = open("/data/SEMDEMO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 19;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 20;
    }
    if (close(fd) != 0)
        return 21;
    return 0;
}
