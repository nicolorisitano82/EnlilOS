/*
 * net_outbound — smoke test guest -> host via QEMU SLIRP
 *
 * Connect to 10.0.2.2:8081, issue a tiny HTTP GET and write the first
 * response bytes to /data/NETOUT.TXT.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#define NETOUT_HOST 0x0A000202U /* 10.0.2.2 */
#define NETOUT_PORT 8081

static void append_line(const char *line)
{
    int fd = open("/data/NETOUT.TXT", O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0)
        return;
    write(fd, line, strlen(line));
    close(fd);
}

int main(void)
{
    struct sockaddr_in sa;
    char               req[] = "GET /health HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    char               buf[128];
    int                fd;
    int                n;

    fd = open("/data/NETOUT.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        append_line("socket-fail\n");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(NETOUT_PORT);
    sa.sin_addr.s_addr = htonl(NETOUT_HOST);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        append_line("connect-fail\n");
        close(fd);
        return 2;
    }

    if (send(fd, req, sizeof(req) - 1U, 0) < 0) {
        append_line("send-fail\n");
        close(fd);
        return 3;
    }

    n = (int)recv(fd, buf, sizeof(buf) - 1U, 0);
    if (n <= 0) {
        append_line("recv-fail\n");
        close(fd);
        return 4;
    }

    buf[n] = '\0';
    append_line("ok\n");
    append_line(buf);
    close(fd);
    return 0;
}
