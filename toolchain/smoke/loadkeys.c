#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../include/user_svc.h"

#define SYS_KBD_SET_LAYOUT 74
#define SYS_KBD_GET_LAYOUT 75

static long sys_kbd_set_layout(const char *name)
{
    return user_svc1(SYS_KBD_SET_LAYOUT, (long)name);
}

static long sys_kbd_get_layout(char *buf, unsigned long len)
{
    return user_svc2(SYS_KBD_GET_LAYOUT, (long)buf, (long)len);
}

static int persist_layout(const char *name)
{
    int  fd;
    char buf[64];
    int  len;

    if (mkdir("/data/etc", 0755) < 0 && errno != EEXIST)
        return -1;

    fd = open("/data/etc/vconsole.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    len = snprintf(buf, sizeof(buf), "KEYMAP=%s\n", name);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    if (write(fd, buf, (size_t)len) != len) {
        int saved = errno;
        close(fd);
        errno = saved ? saved : EIO;
        return -1;
    }

    if (close(fd) < 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    char layout[16];

    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        fprintf(stderr, "usage: loadkeys <us|it|/usr/share/kbd/keymaps/*.map>\n");
        return 1;
    }

    if (sys_kbd_set_layout(argv[1]) < 0) {
        fprintf(stderr, "loadkeys: layout non supportato: %s\n", argv[1]);
        return 1;
    }

    if (sys_kbd_get_layout(layout, sizeof(layout)) < 0)
        strncpy(layout, argv[1], sizeof(layout) - 1U);
    layout[sizeof(layout) - 1U] = '\0';

    if (persist_layout(layout) < 0)
        fprintf(stderr, "loadkeys: layout attivo (%s), persistenza non salvata\n", layout);
    else
        printf("layout attivo: %s\n", layout);

    return 0;
}
