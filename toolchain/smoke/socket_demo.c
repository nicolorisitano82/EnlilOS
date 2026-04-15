/*
 * socket_demo — TCP loopback echo test (M10-03)
 *
 * Fork server+client su 127.0.0.1:7070.
 * Server: listen, accept, recv "hello\n", send "hello\n", esce.
 * Client: yield qualche volta per dare tempo al server di fare listen,
 *         connect, send "hello\n", recv "hello\n", verifica.
 *
 * Output atteso in /data/SOCKDEMO.TXT:
 *   sockopt-ok\n
 *   server-ok\n
 *   client-ok\n
 *   udp-ok\n
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#define SERVER_PORT  7070
#define UDP_PORT     7071
#define MSG          "hello\n"
#define MSG_LEN      6
#define UDP_MSG      "udp\n"
#define UDP_MSG_LEN  4

static void write_result(const char *txt)
{
    int fd = open("/data/SOCKDEMO.TXT",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    write(fd, txt, strlen(txt));
    close(fd);
}

static int run_server(void)
{
    struct sockaddr_in sa;
    char buf[32];
    int  srv, cli, n;

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
        return 1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(SERVER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        goto out;
    if (listen(srv, 4) < 0)
        goto out;

    cli = accept(srv, NULL, NULL);
    if (cli < 0)
        goto out;

    n = (int)recv(cli, buf, sizeof(buf) - 1, 0);
    if (n != MSG_LEN || memcmp(buf, MSG, MSG_LEN) != 0) {
        close(cli);
        goto out;
    }

    if (send(cli, MSG, MSG_LEN, 0) != MSG_LEN) {
        close(cli);
        goto out;
    }

    close(cli);
    close(srv);
    return 0;
out:
    close(srv);
    return 1;
}

static int run_udp(void)
{
    struct sockaddr_in rx_sa;
    struct sockaddr_in src_sa;
    struct sockaddr_in dst_sa;
    socklen_t          src_len = sizeof(src_sa);
    char               buf[32];
    int                rx, tx, on = 1;
    int                got = 0;
    socklen_t          got_len = sizeof(got);
    ssize_t            n;
    int                result = 0;

    rx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0)
        return 0;
    tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0) {
        close(rx);
        return 0;
    }

    if (setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        goto out;
    if (getsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &got, &got_len) < 0)
        goto out;
    if (got != 1 || got_len != sizeof(got))
        goto out;
    result |= 1;

    memset(&rx_sa, 0, sizeof(rx_sa));
    rx_sa.sin_family      = AF_INET;
    rx_sa.sin_port        = htons(UDP_PORT);
    rx_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(rx, (struct sockaddr *)&rx_sa, sizeof(rx_sa)) < 0)
        goto out;

    memset(&dst_sa, 0, sizeof(dst_sa));
    dst_sa.sin_family      = AF_INET;
    dst_sa.sin_port        = htons(UDP_PORT);
    dst_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sendto(tx, UDP_MSG, UDP_MSG_LEN, 0,
               (struct sockaddr *)&dst_sa, sizeof(dst_sa)) != UDP_MSG_LEN)
        goto out;

    n = recvfrom(rx, buf, sizeof(buf), 0, (struct sockaddr *)&src_sa, &src_len);
    if (n != UDP_MSG_LEN || memcmp(buf, UDP_MSG, UDP_MSG_LEN) != 0)
        goto out;
    if (src_sa.sin_family != AF_INET)
        goto out;

    result |= 2;
out:
    close(tx);
    close(rx);
    return result;
}

static int run_client(void)
{
    struct sockaddr_in sa;
    char buf[32];
    int  sock, n;
    int  retries = 50;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return 1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(SERVER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* Ritenta il connect finché il server non è in ascolto */
    while (retries-- > 0) {
        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0)
            break;
        if (errno != ECONNREFUSED)
            goto out;
        /* yield cooperativo — lascia girare il server */
        /* Aspetta un po' e riprova */
        close(sock);
        {
            struct timespec ts = { 0, 2000000L };   /* 2 ms */
            nanosleep(&ts, NULL);
        }
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return 1;
    }
    if (retries < 0)
        goto out;

    if (send(sock, MSG, MSG_LEN, 0) != MSG_LEN)
        goto out;

    n = (int)recv(sock, buf, sizeof(buf) - 1, 0);
    if (n != MSG_LEN || memcmp(buf, MSG, MSG_LEN) != 0)
        goto out;

    close(sock);
    return 0;
out:
    close(sock);
    return 1;
}

int main(void)
{
    pid_t pid;
    int   status;
    int   server_ok;
    int   udp_status;

    /* Svuota il file di output */
    int fd = open("/data/SOCKDEMO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);

    pid = fork();
    if (pid < 0)
        return 1;

    if (pid == 0) {
        /* Figlio = client: breve pausa per dare tempo al server */
        struct timespec ts = { 0, 5000000L };   /* 5 ms */
        nanosleep(&ts, NULL);
        _exit(run_client() == 0 ? 0 : 1);
    }

    /* Padre = server */
    server_ok = run_server();
    waitpid(pid, &status, 0);
    udp_status = run_udp();

    if (udp_status & 1)
        write_result("sockopt-ok\n");
    if (server_ok == 0)
        write_result("server-ok\n");
    if ((status & 0x7FU) == 0 && ((status >> 8) & 0xFFU) == 0)
        write_result("client-ok\n");
    if (udp_status & 2)
        write_result("udp-ok\n");
    return 0;
}
