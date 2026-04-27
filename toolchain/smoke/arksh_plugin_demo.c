/*
 * arksh_plugin_demo — smoke test M8-08 plugin v1
 *
 * Verifica:
 *   1. dlopen("/usr/lib/arksh/plugins/enlil.so")  → handle valido
 *   2. dlsym → arksh_plugin_query non NULL
 *   3. query()  → abi_major == ARKSH_PLUGIN_ABI_MAJOR, name non vuoto
 *   4. dlsym → arksh_plugin_init non NULL
 *   5. init() con ArkshPluginHost stub → ritorna 0
 *   6. dlclose → OK
 *
 * Output: /data/ARKSHPLUGIN.TXT = "arksh-plugin-ok\n"
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "arksh/plugin.h"

/* ------------------------------------------------------------------ */
/* Stub ArkshPluginHost — funzioni no-op, struttura ABI-compatibile    */
/* ------------------------------------------------------------------ */

static int stub_register_command(ArkshShell *s, const char *n,
                                  const char *d, ArkshCommandFn f)
{
    (void)s; (void)n; (void)d; (void)f;
    return 0;
}

static int stub_register_prop(ArkshShell *s, const char *t,
                               const char *n, ArkshExtensionPropertyFn f)
{
    (void)s; (void)t; (void)n; (void)f;
    return 0;
}

static int stub_register_method(ArkshShell *s, const char *t,
                                 const char *n, ArkshExtensionMethodFn f)
{
    (void)s; (void)t; (void)n; (void)f;
    return 0;
}

static int stub_register_resolver(ArkshShell *s, const char *n,
                                   const char *d, ArkshValueResolverFn f)
{
    (void)s; (void)n; (void)d; (void)f;
    return 0;
}

static int stub_register_stage(ArkshShell *s, const char *n,
                                const char *d, ArkshPipelineStageFn f)
{
    (void)s; (void)n; (void)d; (void)f;
    return 0;
}

static int stub_register_type(ArkshShell *s, const char *n, const char *d)
{
    (void)s; (void)n; (void)d;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void write_result(const char *msg)
{
    int fd = open("/data/ARKSHPLUGIN.TXT",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        close(fd);
    }
}

static void write_error(const char *prefix, const char *detail)
{
    int fd = open("/data/ARKSHPLUGIN.TXT",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (prefix)
        write(fd, prefix, strlen(prefix));
    if (detail)
        write(fd, detail, strlen(detail));
    close(fd);
}

static void fail(void *handle, const char *reason)
{
    char *err = dlerror();
    if (err && err[0] != '\0')
        write_error(reason, err);
    else
        write_result(reason);
    if (handle)
        dlclose(handle);
    _exit(1);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    void                *handle;
    ArkshPluginQueryFn   query_fn;
    ArkshPluginInitFn    init_fn;
    ArkshPluginInfo      info;
    ArkshPluginHost      host;

    /* ── 1. dlopen ──────────────────────────────────────────────── */
    handle = dlopen("/usr/lib/arksh/plugins/enlil.so", RTLD_NOW);
    if (!handle)
        fail(NULL, "dlopen-fail\n");

    /* ── 2. dlsym arksh_plugin_query ────────────────────────────── */
    *(void **)&query_fn = dlsym(handle, "arksh_plugin_query");
    if (!query_fn)
        fail(handle, "dlsym-query-fail\n");

    /* ── 3. query: ABI version + name non vuoto ─────────────────── */
    memset(&info, 0, sizeof(info));
    if (query_fn(&info) != 0)
        fail(handle, "query-error\n");
    if (info.abi_major != ARKSH_PLUGIN_ABI_MAJOR)
        fail(handle, "query-abi-mismatch\n");
    if (info.name[0] == '\0')
        fail(handle, "query-empty-name\n");

    /* ── 4. dlsym arksh_plugin_init ─────────────────────────────── */
    *(void **)&init_fn = dlsym(handle, "arksh_plugin_init");
    if (!init_fn)
        fail(handle, "dlsym-init-fail\n");

    /* ── 5. init con host stub ──────────────────────────────────── */
    memset(&host, 0, sizeof(host));
    host.api_version             = ARKSH_PLUGIN_ABI_MAJOR;
    host.abi_major               = ARKSH_PLUGIN_ABI_MAJOR;
    host.abi_minor               = ARKSH_PLUGIN_ABI_MINOR;
    host.capability_flags        = ARKSH_PLUGIN_CAP_ALL_CURRENT;
    host.register_command        = stub_register_command;
    host.register_property_extension = stub_register_prop;
    host.register_method_extension   = stub_register_method;
    host.register_value_resolver = stub_register_resolver;
    host.register_pipeline_stage = stub_register_stage;
    host.register_type_descriptor = stub_register_type;

    /* ArkshShell* opaco — il plugin non lo dereferenzia nel comando registrato.
     * Passiamo &host come dummy non-NULL valido. */
    memset(&info, 0, sizeof(info));
    if (init_fn((ArkshShell *)&host, &host, &info) != 0)
        fail(handle, "init-fail\n");

    /* ── 6. dlclose ─────────────────────────────────────────────── */
    if (dlclose(handle) != 0)
        fail(NULL, "dlclose-fail\n");

    write_result("arksh-plugin-ok\n");
    return 0;
}
