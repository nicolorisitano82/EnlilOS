/*
 * pty_demo — PTY (pseudo-terminal) smoke test (M11-05f)
 *
 * Verifica:
 *   1. posix_openpt / grantpt / unlockpt / ptsname_r
 *   2. open("/dev/pts/N") → slave fd
 *   3. isatty(slave) = 1, isatty(master) = 0
 *   4. tcgetattr su slave → OK
 *   5. tcsetattr su slave (raw mode)
 *   6. write(master) → read(slave): trasmissione raw
 *   7. write(slave) → read(master): trasmissione raw
 *   8. TIOCGWINSZ su slave → 80×25 default
 *   9. TIOCSWINSZ su master → aggiorna winsize
 *  10. TIOCGWINSZ su slave → verifica nuova dimensione
 *  11. close entrambi
 *
 * Output: /data/PTYDEMO.TXT = "pty-demo-ok\n"
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

static void write_result(const char *msg)
{
    int fd = open("/data/PTYDEMO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        close(fd);
    }
}

static void fail(const char *reason)
{
    write_result(reason);
    exit(1);
}

int main(void)
{
    int            master_fd, slave_fd;
    char           slave_path[32];
    struct termios t;
    struct winsize ws;
    char           buf[16];
    const char    *payload = "hello";
    int            n;

    /* ── 1. apre ptmx ─────────────────────────────────────────── */
    master_fd = posix_openpt(O_RDWR);
    if (master_fd < 0) fail("posix_openpt-fail\n");

    if (grantpt(master_fd) < 0)  fail("grantpt-fail\n");
    if (unlockpt(master_fd) < 0) fail("unlockpt-fail\n");
    if (ptsname_r(master_fd, slave_path, sizeof(slave_path)) != 0)
        fail("ptsname_r-fail\n");

    /* slave_path deve iniziare con /dev/pts/ */
    if (slave_path[0] != '/' || slave_path[1] != 'd')
        fail("ptsname_r-bad-path\n");

    /* ── 2. apre slave ────────────────────────────────────────── */
    slave_fd = open(slave_path, O_RDWR);
    if (slave_fd < 0) fail("slave-open-fail\n");

    /* ── 3. isatty ───────────────────────────────────────────── */
    if (!isatty(slave_fd))  fail("isatty-slave-fail\n");
    if ( isatty(master_fd)) fail("isatty-master-should-be-0\n");

    /* ── 4. tcgetattr ────────────────────────────────────────── */
    if (tcgetattr(slave_fd, &t) < 0) fail("tcgetattr-fail\n");

    /* ── 5. raw mode (disabilita ICANON + ECHO) ─────────────── */
    t.c_lflag &= (unsigned)~(ICANON | ECHO);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(slave_fd, TCSANOW, &t) < 0) fail("tcsetattr-fail\n");

    /* ── 6. write(master) → read(slave) ─────────────────────── */
    n = (int)write(master_fd, payload, 5);
    if (n != 5) fail("master-write-fail\n");

    n = (int)read(slave_fd, buf, sizeof(buf));
    if (n != 5 || memcmp(buf, payload, 5) != 0) fail("slave-read-fail\n");

    /* ── 7. write(slave) → read(master) ─────────────────────── */
    /* In raw mode senza OPOST/ONLCR: scrive "world" (5B) → master legge "world" */
    /* (disabilita anche OPOST per test puro) */
    t.c_oflag &= (unsigned)~(OPOST);
    tcsetattr(slave_fd, TCSANOW, &t);

    n = (int)write(slave_fd, "world", 5);
    if (n != 5) fail("slave-write-fail\n");

    n = (int)read(master_fd, buf, sizeof(buf));
    if (n != 5 || memcmp(buf, "world", 5) != 0) fail("master-read-fail\n");

    /* ── 8. TIOCGWINSZ su slave → 80×25 ─────────────────────── */
    if (ioctl(slave_fd, TIOCGWINSZ, &ws) < 0) fail("tiocgwinsz-fail\n");
    if (ws.ws_col != 80 || ws.ws_row != 25)    fail("tiocgwinsz-wrong-default\n");

    /* ── 9. TIOCSWINSZ su master ─────────────────────────────── */
    ws.ws_col = 120; ws.ws_row = 40;
    ws.ws_xpixel = 0; ws.ws_ypixel = 0;
    if (ioctl(master_fd, TIOCSWINSZ, &ws) < 0) fail("tiocswinsz-fail\n");

    /* ── 10. TIOCGWINSZ su slave → verifica ─────────────────── */
    {
        struct winsize ws2;
        if (ioctl(slave_fd, TIOCGWINSZ, &ws2) < 0)        fail("tiocgwinsz2-fail\n");
        if (ws2.ws_col != 120 || ws2.ws_row != 40)         fail("tiocgwinsz2-wrong-value\n");
    }

    /* ── 11. chiudi ──────────────────────────────────────────── */
    close(slave_fd);
    close(master_fd);

    write_result("pty-demo-ok\n");
    return 0;
}
