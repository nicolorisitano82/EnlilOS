#ifndef ENLILOS_MUSL_SYS_UTSNAME_H
#define ENLILOS_MUSL_SYS_UTSNAME_H

#define UTSNAME_FIELD_LEN 65

struct utsname {
    char sysname[UTSNAME_FIELD_LEN];
    char nodename[UTSNAME_FIELD_LEN];
    char release[UTSNAME_FIELD_LEN];
    char version[UTSNAME_FIELD_LEN];
    char machine[UTSNAME_FIELD_LEN];
    char domainname[UTSNAME_FIELD_LEN];
};

int uname(struct utsname *buf);

#endif
