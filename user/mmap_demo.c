/*
 * EnlilOS — mmap_demo (M8-02)
 *
 * Programma EL0 che testa mmap() file-backed:
 *   1. MAP_PRIVATE su /data/mmap_priv.dat — legge il contenuto
 *   2. MAP_SHARED  su /data/mmap_shrd.dat — scrive e chiama msync()
 *
 * Esce con 0 se tutti i test passano, -1 altrimenti.
 */

#include "user_svc.h"
#include "syscall.h"

/* Costanti SVC inlining */
#define SYS_WRITE_NR    1
#define SYS_EXIT_NR     3
#define SYS_OPEN_NR     4
#define SYS_CLOSE_NR    5
#define SYS_MMAP_NR     7
#define SYS_MUNMAP_NR   8
#define SYS_MSYNC_NR    24

#define PROT_READ_  (1)
#define PROT_WRITE_ (2)
#define MAP_SHARED_  (1)
#define MAP_PRIVATE_ (2)
#define MS_SYNC_    4

#define PAGE_SIZE_ 4096UL

/* ── Utilità minimali ─────────────────────────────────────────────── */

static int my_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int my_memcmp(const void *a, const void *b, int n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for (int i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

static void my_memcpy(void *dst, const void *src, int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void say(const char *s)
{
    int len = my_strlen(s);
    user_svc3(SYS_WRITE_NR, 1, (long)s, (long)len);
}

static void die(const char *reason)
{
    say("[MMAPD] FAIL: ");
    say(reason);
    say("\n");
    user_svc_exit(-1, SYS_EXIT_NR);
}

/* ── Entrypoint ───────────────────────────────────────────────────── */

void _start(void)
{
    /* Contenuto atteso del file privato (scritto dal kernel selftest) */
    static const char priv_magic[] = "MMAP_PRIVATE_OK";
    /* Scrittura nel file condiviso */
    static const char shrd_write[]  = "MMAP_SHARED_OK!";

    long fd;
    long mmap_va;
    long rc;

    /* ── Test 1: MAP_PRIVATE ──────────────────────────────────────── */

    fd = user_svc2(SYS_OPEN_NR, (long)"/data/mmap_priv.dat", O_RDONLY);
    if (fd < 0)
        die("open mmap_priv.dat");

    /* mmap MAP_PRIVATE | PROT_READ — lunghezza 1 pagina */
    mmap_va = user_svc6(SYS_MMAP_NR,
                        0,               /* hint */
                        PAGE_SIZE_,      /* length */
                        PROT_READ_,
                        MAP_PRIVATE_,
                        fd,
                        0);              /* offset */

    if (mmap_va < 0 || mmap_va == (long)(unsigned long)(-1UL))
        die("mmap MAP_PRIVATE");

    /* Legge il contenuto dalla mappatura */
    if (my_memcmp((void *)mmap_va, priv_magic, my_strlen(priv_magic)) != 0)
        die("contenuto MAP_PRIVATE non corrisponde");

    rc = user_svc2(SYS_MUNMAP_NR, mmap_va, PAGE_SIZE_);
    (void)rc;

    user_svc1(SYS_CLOSE_NR, fd);

    /* ── Test 2: MAP_SHARED + msync ──────────────────────────────── */

    fd = user_svc2(SYS_OPEN_NR, (long)"/data/mmap_shrd.dat",
                   O_RDWR);
    if (fd < 0)
        die("open mmap_shrd.dat");

    mmap_va = user_svc6(SYS_MMAP_NR,
                        0,
                        PAGE_SIZE_,
                        PROT_READ_ | PROT_WRITE_,
                        MAP_SHARED_,
                        fd,
                        0);

    if (mmap_va < 0 || mmap_va == (long)(unsigned long)(-1UL))
        die("mmap MAP_SHARED");

    /* Sovrascrive il contenuto della pagina */
    my_memcpy((void *)mmap_va, shrd_write, my_strlen(shrd_write) + 1);

    /* msync — flush al file */
    rc = user_svc3(SYS_MSYNC_NR, mmap_va, PAGE_SIZE_, MS_SYNC_);
    if (rc != 0)
        die("msync fallito");

    rc = user_svc2(SYS_MUNMAP_NR, mmap_va, PAGE_SIZE_);
    (void)rc;

    user_svc1(SYS_CLOSE_NR, fd);

    /* ── Successo ─────────────────────────────────────────────────── */
    say("[MMAPD] Tutti i test mmap superati\n");
    user_svc_exit(0, SYS_EXIT_NR);
}
