#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <toon.h>

#define ENLIL_RUN_PATH_MAX 256U
#define ENLIL_RUN_ARGV_MAX 16U

extern char **environ;

static int join_path(char *out, size_t cap, const char *base, const char *suffix)
{
    int need_slash;

    if (!out || !base || !suffix || cap == 0U)
        return -1;
    need_slash = (base[0] != '\0' && base[strlen(base) - 1U] != '/') ? 1 : 0;
    if (snprintf(out, cap, "%s%s%s", base, need_slash ? "/" : "", suffix) >= (int)cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "uso: enlil-run /NomeApp.enlil [arg ...]\n");
}

int main(int argc, char **argv)
{
    toon_doc_t    doc;
    char          manifest[ENLIL_RUN_PATH_MAX];
    char          entry_path[ENLIL_RUN_PATH_MAX];
    const char   *entry;
    char         *child_argv[ENLIL_RUN_ARGV_MAX];
    int           child_argc = 0;

    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        usage();
        return 2;
    }

    if (join_path(manifest, sizeof(manifest), argv[1], "manifest.toon") < 0) {
        perror("enlil-run: manifest path");
        return 2;
    }
    if (toon_parse_file(manifest, &doc) < 0) {
        fprintf(stderr, "enlil-run: parse %s fallita: %s\n",
                manifest, doc.error[0] ? doc.error : "errore generico");
        return 3;
    }

    entry = toon_get_string(&doc, "entry");
    if (!entry || entry[0] == '\0') {
        fprintf(stderr, "enlil-run: campo entry mancante in %s\n", manifest);
        return 4;
    }
    if (join_path(entry_path, sizeof(entry_path), argv[1], entry) < 0) {
        perror("enlil-run: entry path");
        return 5;
    }
    if (setenv("ENLIL_BUNDLE_ROOT", argv[1], 1) < 0) {
        perror("enlil-run: setenv");
        return 6;
    }

    child_argv[child_argc++] = entry_path;
    for (int i = 2; i < argc && child_argc < ENLIL_RUN_ARGV_MAX - 1; i++)
        child_argv[child_argc++] = argv[i];
    child_argv[child_argc] = NULL;

    execve(entry_path, child_argv, environ);
    perror("enlil-run: execve");
    return 7;
}
