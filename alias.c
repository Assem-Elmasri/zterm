
#include "zterm.h"

Alias g_aliases[MAX_ALIASES];
int g_alias_count = 0;

Alias *alias_find(const char *name) {
  for (int i = 0; i < g_alias_count; i++)
    if (strcmp(g_aliases[i].name, name) == 0)
      return &g_aliases[i];
  return NULL;
}

void alias_expand(Command *cmd) {
  if (cmd->argc == 0)
    return;

  Alias *a = alias_find(cmd->args[0]);
  if (!a)
    return;

  char newline[MAX_INPUT];
  snprintf(newline, sizeof(newline), "%s", a->value);
  // add args
  for (int i = 1; i < cmd->argc; i++) {
    strncat(newline, " ", sizeof(newline) - strlen(newline) - 1);
    strncat(newline, cmd->args[i], sizeof(newline) - strlen(newline) - 1);
  }
  // free mem
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
