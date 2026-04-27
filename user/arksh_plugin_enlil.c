/*
 * arksh_plugin_enlil — EnlilOS native plugin per arksh (M8-08 plugin v1)
 *
 * Esporta:
 *   arksh_plugin_query    — metadati: name="enlil", abi=5.0
 *   arksh_plugin_init     — registra comando "enlil-info"
 *   arksh_plugin_shutdown — no-op
 *
 * Comandi registrati:
 *   enlil-info            — stampa info OS: name, arch, kernel, abi
 *
 * Freestanding: nessuna dipendenza da libc.
 * Un eseguibile statico musl non esporta .dynsym, quindi memset/snprintf/strncpy
 * non sarebbero risolubili a dlopen-time.  Usiamo helper inline e string literal.
 *
 * Compilare con aarch64-elf-gcc -fPIC -I<arksh>/include -c
 * Linkare con aarch64-elf-gcc -shared -nostdlib -Wl,-T,user/user_shared.ld
 */

#include "arksh/plugin.h"

/* ------------------------------------------------------------------ */
/* Helpers freestanding (no libc)                                       */
/* ------------------------------------------------------------------ */

static void enlil_bzero(void *dst, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = 0;
}

/* Copy up to n-1 bytes + NUL-terminate; like strlcpy semantics */
static void enlil_copystr(char *dst, const char *src, size_t n)
{
    size_t i;
    if (!n) return;
    for (i = 0U; i < n - 1U && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Info string: compile-time constant, avoids snprintf                 */
/* ------------------------------------------------------------------ */

/*
 * Stringify compile-time macro value.
 * ARKSH_PLUGIN_ABI_MAJOR = 5, ARKSH_PLUGIN_ABI_MINOR = 0  → "5.0"
 */
#define ENLIL_STR2(x)  #x
#define ENLIL_STR(x)   ENLIL_STR2(x)

static const char enlil_info_str[] =
    "OS: EnlilOS  arch: aarch64  kernel: 0.11"
    "  libc: musl-bootstrap"
    "  plugin-abi: "
    ENLIL_STR(ARKSH_PLUGIN_ABI_MAJOR) "." ENLIL_STR(ARKSH_PLUGIN_ABI_MINOR);

static int enlil_fill_plugin_info(ArkshPluginInfo *info)
{
    if (!info)
        return 1;

    enlil_bzero(info, sizeof(*info));
    enlil_copystr(info->name, "enlil", sizeof(info->name));
    enlil_copystr(info->version, "1.0.0", sizeof(info->version));
    enlil_copystr(info->description,
                  "EnlilOS native plugin: system info commands",
                  sizeof(info->description));

    info->abi_major                  = ARKSH_PLUGIN_ABI_MAJOR;
    info->abi_minor                  = ARKSH_PLUGIN_ABI_MINOR;
    info->required_host_capabilities = ARKSH_PLUGIN_CAP_COMMANDS;
    info->plugin_capabilities        = ARKSH_PLUGIN_CAP_COMMANDS;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Comandi                                                              */
/* ------------------------------------------------------------------ */

static int cmd_enlil_info(ArkshShell *shell,
                           int argc, char **argv,
                           char *out, size_t out_size)
{
    (void)shell;
    (void)argc;
    (void)argv;

    if (!out || out_size == 0U)
        return 0;

    /* Copy static string into output buffer */
    size_t i = 0U;
    const char *s = enlil_info_str;
    while (*s && i + 1U < out_size)
        out[i++] = *s++;
    out[i] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point: query (metadati, non richiede ArkshShell)              */
/* ------------------------------------------------------------------ */

ARKSH_PLUGIN_EXPORT int arksh_plugin_query(ArkshPluginInfo *info)
{
    return enlil_fill_plugin_info(info);
}

/* ------------------------------------------------------------------ */
/* Entry point: init                                                    */
/* ------------------------------------------------------------------ */

ARKSH_PLUGIN_EXPORT int arksh_plugin_init(ArkshShell *shell,
                                           const ArkshPluginHost *host,
                                           ArkshPluginInfo *info)
{
    if (!shell || !host || !info)
        return 1;
    if (host->abi_major != ARKSH_PLUGIN_ABI_MAJOR)
        return 1;
    if (!host->register_command)
        return 1;
    if (enlil_fill_plugin_info(info) != 0)
        return 1;

    return host->register_command(shell,
                                  "enlil-info",
                                  "Show EnlilOS system info (OS name, arch, kernel, ABI)",
                                  cmd_enlil_info);
}

/* ------------------------------------------------------------------ */
/* Entry point: shutdown                                                */
/* ------------------------------------------------------------------ */

ARKSH_PLUGIN_EXPORT void arksh_plugin_shutdown(ArkshShell *shell)
{
    (void)shell;
}
