#ifndef ENLILOS_MUSL_SYS_REBOOT_H
#define ENLILOS_MUSL_SYS_REBOOT_H

/* cmd valori (stessi numeri di Linux per compatibilità) */
#define RB_AUTOBOOT         0x01234567
#define RB_HALT_SYSTEM      0xCDEF0123
#define RB_POWER_OFF        0x4321FEDC

/* Alias Linux-style */
#define LINUX_REBOOT_CMD_RESTART    RB_AUTOBOOT
#define LINUX_REBOOT_CMD_HALT       RB_HALT_SYSTEM
#define LINUX_REBOOT_CMD_POWER_OFF  RB_POWER_OFF

int reboot(int cmd);

#endif /* ENLILOS_MUSL_SYS_REBOOT_H */
