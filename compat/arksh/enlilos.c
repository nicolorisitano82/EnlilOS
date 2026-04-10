/*
 * Reference shim for the EnlilOS arksh port.
 *
 * This file is not compiled by EnlilOS directly. It documents the minimal
 * platform glue expected by M8-08e and can be adapted upstream once the
 * arksh source tree is available locally.
 */

#include <stdlib.h>

const char *enlilos_xdg_config_home(void)
{
    const char *v = getenv("XDG_CONFIG_HOME");
    return (v && *v) ? v : "/home/user/.config";
}

const char *enlilos_xdg_state_home(void)
{
    const char *v = getenv("XDG_STATE_HOME");
    return (v && *v) ? v : "/home/user/.local/state";
}

const char *enlilos_default_shell_path(void)
{
    return "/bin/arksh";
}

const char *enlilos_default_term(void)
{
    return "vt100";
}
