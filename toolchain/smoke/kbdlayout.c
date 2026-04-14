#include <stdio.h>

#include "../../include/user_svc.h"

#define SYS_KBD_GET_LAYOUT 75

static long sys_kbd_get_layout(char *buf, unsigned long len)
{
    return user_svc2(SYS_KBD_GET_LAYOUT, (long)buf, (long)len);
}

int main(void)
{
    char layout[16];

    if (sys_kbd_get_layout(layout, sizeof(layout)) < 0) {
        fputs("kbdlayout: query fallita\n", stderr);
        return 1;
    }

    puts(layout);
    return 0;
}
