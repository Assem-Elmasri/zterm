/*
 * alias.c — Alias table: find, expand, create, and remove aliases.
 *
 * alias_expand() rewrites the first word of a Command in-place whenever
 * it matches a known alias, re-parsing the expanded string back into args.
 */

#include "zterm.h"

/* ── Global definitions ────────────────────────────────────────────────── */
Alias g_aliases[MAX_ALIASES];
int   g_alias_count = 0;

/* ══════════════════════════════════════════════════════════════════════════
   Implementation
   ══════════════════════════════════════════════════════════════════════════ */

Alias *alias_find(const char *name)
{
    for (int i = 0; i < g_alias_count; i++)
        if (strcmp(g_aliases[i].name, name) == 0)
            return &g_aliases[i];
    return NULL;
}

void alias_expand(Command *cmd)
{
    if (cmd->argc == 0)
        return;

    Alias *a = alias_find(cmd->args[0]);
    if (!a)
        return;

    /* Build expanded command line: alias value + original remaining args */
    char newline[MAX_INPUT];
    snprintf(newline, sizeof(newline), "%s", a->value);
    for (int i = 1; i < cmd->argc; i++) {
        strncat(newline, " ",         sizeof(newline) - strlen(newline) - 1);
        strncat(newline, cmd->args[i], sizeof(newline) - strlen(newline) - 1);
    }

    /* Free old argument list */
    for (int i = 0; i < cmd->argc; i++) {
        free(cmd->args[i]);
        cmd->args[i] = NULL;
    }
    cmd->argc = 0;

    /* Re-parse the expanded line back into this Command */
    Pipeline tmp;
    parse_pipeline(newline, &tmp);
    if (tmp.count > 0) {
        Command *tc = &tmp.cmds[0];
        for (int i = 0; i < tc->argc; i++)
            cmd->args[cmd->argc++] = strdup(tc->args[i]);
        cmd->args[cmd->argc] = NULL;
    }
}
