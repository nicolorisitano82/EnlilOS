/*
 * EnlilOS — /sbin/poweroff
 *
 * Syncs filesystem (via SYS_REBOOT che fa la sequenza kernel-side)
 * e spegne la macchina con PSCI SYSTEM_OFF.
 *
 * Uso: poweroff [-r|--reboot] [-h|--halt]
 */

#include <sys/reboot.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    int cmd = RB_POWER_OFF;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reboot") == 0)
            cmd = RB_AUTOBOOT;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--halt") == 0)
            cmd = RB_HALT_SYSTEM;
        else {
            fprintf(stderr, "Uso: poweroff [-r|--reboot] [-h|--halt]\n");
            return 1;
        }
    }

    switch (cmd) {
    case RB_AUTOBOOT:
        fprintf(stderr, "Riavvio in corso...\n");
        break;
    case RB_HALT_SYSTEM:
        fprintf(stderr, "Halt in corso...\n");
        break;
    default:
        fprintf(stderr, "Spegnimento in corso...\n");
        break;
    }

    if (reboot(cmd) < 0) {
        perror("reboot");
        return 1;
    }
    return 0;
}
