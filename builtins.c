/*
 * builtins.c — Shell built-in commands and their dispatcher.
 *
 * Built-ins:  cd  pwd  echo  export  unset  env  history
 *             alias  unalias  type  help  source  true  false  exit
 *
 * try_builtin() is the single entry-point used by the executor.  It
 * returns the exit status (≥ 0) when a built-in was matched, or -1 when
 * the command is not a built-in (caller should exec an external binary).
 */

#include "zterm.h"

/* ══════════════════════════════════════════════════════════════════════════
   cd
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_cd(Command *cmd) {
  const char *path;

  if (cmd->argc == 1) {
    path = getenv("HOME");
    if (!path) {
      fprintf(stderr, "cd: HOME not set\n");
      return 1;
    }
  } else if (strcmp(cmd->args[1], "-") == 0) {
    path = getenv("OLDPWD");
    if (!path) {
      fprintf(stderr, "cd: OLDPWD not set\n");
      return 1;
    }
    printf("%s\n", path);
  } else {
    path = cmd->args[1];
  }

  char cwd[512];
  getcwd(cwd, sizeof(cwd));

  if (chdir(path) != 0) {
    fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
    return 1;
  }

  setenv("OLDPWD", cwd, 1);

  char ncwd[512];
  getcwd(ncwd, sizeof(ncwd));
  setenv("PWD", ncwd, 1);

  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   pwd
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_pwd(Command *cmd) {
  (void)cmd;
  char cwd[512];
  if (getcwd(cwd, sizeof(cwd))) {
    printf("%s\n", cwd);
    return 0;
  }
  perror("pwd");
  return 1;
}

/* ══════════════════════════════════════════════════════════════════════════
   echo
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_echo(Command *cmd) {
  int newline = 1;
  int interpret = 0;
  int start = 1;

  if (cmd->argc > 1 && strcmp(cmd->args[1], "-n") == 0) {
    newline = 0;
    start = 2;
  }
  if (cmd->argc > start && strcmp(cmd->args[start], "-e") == 0) {
    interpret = 1;
    start++;
  }

  for (int i = start; i < cmd->argc; i++) {
    if (i > start)
      putchar(' ');

    if (interpret) {
      const char *p = cmd->args[i];
      while (*p) {
        if (*p == '\\') {
          p++;
          switch (*p) {
          case 'n':
            putchar('\n');
            break;
          case 't':
            putchar('\t');
            break;
          case '\\':
            putchar('\\');
            break;
          case 'r':
            putchar('\r');
            break;
          case '0':
            putchar('\0');
            break;
          default:
            putchar('\\');
            putchar(*p);
            break;
          }
          p++;
        } else {
          putchar(*p++);
        }
      }
    } else {
      fputs(cmd->args[i], stdout);
    }
  }

  if (newline)
    putchar('\n');
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   export
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_export(Command *cmd) {
  if (cmd->argc == 1) {
    extern char **environ;
    for (char **e = environ; *e; e++)
      printf("declare -x %s\n", *e);
    return 0;
  }

  for (int i = 1; i < cmd->argc; i++) {
    char *eq = strchr(cmd->args[i], '=');
    if (eq) {
      *eq = '\0';
      setenv(cmd->args[i], eq + 1, 1);
      *eq = '=';
    } else {
      const char *v = getenv(cmd->args[i]);
      if (v)
        setenv(cmd->args[i], v, 1);
    }
  }
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   unset
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_unset(Command *cmd) {
  for (int i = 1; i < cmd->argc; i++)
    unsetenv(cmd->args[i]);
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   env
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_env(Command *cmd) {
  (void)cmd;
  extern char **environ;
  for (char **e = environ; *e; e++)
    printf("%s\n", *e);
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   history
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_history(Command *cmd) {
  int n = g_history.count;
  if (cmd->argc > 1)
    n = atoi(cmd->args[1]);

  int start = g_history.count - n;
  if (start < 0)
    start = 0;

  for (int i = start; i < g_history.count; i++)
    printf("%5d  %s\n", i + 1, g_history.entries[i]);

  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   alias / unalias
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_alias(Command *cmd) {
  if (cmd->argc == 1) {
    for (int i = 0; i < g_alias_count; i++)
      printf("alias %s='%s'\n", g_aliases[i].name, g_aliases[i].value);
    return 0;
  }

  for (int i = 1; i < cmd->argc; i++) {
    char *eq = strchr(cmd->args[i], '=');
    if (!eq) {
      Alias *a = alias_find(cmd->args[i]);
      if (a)
        printf("alias %s='%s'\n", a->name, a->value);
      else
        fprintf(stderr, "alias: %s: not found\n", cmd->args[i]);
      continue;
    }

    *eq = '\0';
    char *name = cmd->args[i];
    char *val = eq + 1;

    /* Strip surrounding quotes */
    size_t vlen = strlen(val);
    if (vlen >= 2 && (val[0] == '\'' || val[0] == '"') &&
        val[vlen - 1] == val[0]) {
      val[vlen - 1] = '\0';
      val++;
    }

    Alias *a = alias_find(name);
    if (a) {
      free(a->value);
      a->value = strdup(val);
    } else if (g_alias_count < MAX_ALIASES) {
      g_aliases[g_alias_count].name = strdup(name);
      g_aliases[g_alias_count].value = strdup(val);
      g_alias_count++;
    }

    *eq = '=';
  }
  return 0;
}

static int builtin_unalias(Command *cmd) {
  for (int i = 1; i < cmd->argc; i++) {
    for (int j = 0; j < g_alias_count; j++) {
      if (strcmp(g_aliases[j].name, cmd->args[i]) == 0) {
        free(g_aliases[j].name);
        free(g_aliases[j].value);
        memmove(&g_aliases[j], &g_aliases[j + 1],
                (g_alias_count - j - 1) * sizeof(Alias));
        g_alias_count--;
        break;
      }
    }
  }
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   type
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_type(Command *cmd) {
  for (int i = 1; i < cmd->argc; i++) {
    const char *name = cmd->args[i];

    Alias *a = alias_find(name);
    if (a) {
      printf("%s is aliased to '%s'\n", name, a->value);
      continue;
    }

    const char *path_env = getenv("PATH");
    if (!path_env)
      path_env = "/usr/local/bin:/usr/bin:/bin";

    char pathcopy[MAX_INPUT];
    strncpy(pathcopy, path_env, sizeof(pathcopy));

    int found = 0;
    char *dir = strtok(pathcopy, ":");
    while (dir) {
      char full[MAX_INPUT];
      snprintf(full, sizeof(full), "%s/%s", dir, name);
      if (access(full, X_OK) == 0) {
        printf("%s is %s\n", name, full);
        found = 1;
        break;
      }
      dir = strtok(NULL, ":");
    }
    if (!found)
      fprintf(stderr, "%s: not found\n", name);
  }
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   help
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_help(Command *cmd) {
  (void)cmd;
  printf("\n%s%smsh%s — Mini Shell  (inspired by bash)\n\n", COLOR_BOLD,
         COLOR_CYAN, COLOR_RESET);

  /* Built-in commands */
  printf("%-20s %s\n", "cd [dir]", "Change directory (- for previous)");
  printf("%-20s %s\n", "pwd", "Print working directory");
  printf("%-20s %s\n", "echo [-n|-e]", "Print arguments");
  printf("%-20s %s\n", "export [K=V]", "Set/show environment variables");
  printf("%-20s %s\n", "unset VAR", "Remove environment variable");
  printf("%-20s %s\n", "env", "Show all environment variables");
  printf("%-20s %s\n", "alias [k=v]", "Create/show aliases");
  printf("%-20s %s\n", "unalias NAME", "Remove alias");
  printf("%-20s %s\n", "history [n]", "Show command history");
  printf("%-20s %s\n", "type CMD", "Show type/location of command");
  printf("%-20s %s\n", "exit [n]", "Exit with status n");
  printf("%-20s %s\n", "help", "Show this help");

  /* Features overview */
  printf("\n%sFeatures:%s\n", COLOR_BOLD, COLOR_RESET);
  printf("  Pipes:          cmd1 | cmd2 | cmd3\n");
  printf("  Redirection:    > >> < 2> &>\n");
  printf("  Background:     cmd &\n");
  printf("  Logical ops:    cmd1 && cmd2   cmd1 || cmd2\n");
  printf("  Glob:           *.c  file?.txt\n");
  printf("  Variables:      $VAR  ${VAR}  $?\n");
  printf("  Cmd subst:      $(cmd)\n");
  printf("  Tilde:          ~/path  ~user/path\n");
  printf("  Line editing:   ←→ cursor, ↑↓ history, Ctrl-A/E/K/U\n\n");
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   source  (also invoked as '.')
   ══════════════════════════════════════════════════════════════════════════ */

static int builtin_source(Command *cmd) {
  if (cmd->argc < 2) {
    fprintf(stderr, "source: filename required\n");
    return 1;
  }

  FILE *f = fopen(cmd->args[1], "r");
  if (!f) {
    fprintf(stderr, "source: %s: %s\n", cmd->args[1], strerror(errno));
    return 1;
  }

  char buf[MAX_INPUT];
  while (fgets(buf, sizeof(buf), f)) {
    buf[strcspn(buf, "\n")] = '\0';
    if (!buf[0] || buf[0] == '#')
      continue;
    Pipeline pl;
    parse_pipeline(buf, &pl);
    g_last_status = shell_execute(&pl);
  }

  fclose(f);
  return g_last_status;
}

/* ══════════════════════════════════════════════════════════════════════════
   Dispatcher
   ══════════════════════════════════════════════════════════════════════════ */

int try_builtin(Command *cmd) {
  if (cmd->argc == 0)
    return 0;

  const char *n = cmd->args[0];

  if (!strcmp(n, "exit")) {
    int code = (cmd->argc > 1) ? atoi(cmd->args[1]) : g_last_status;
    history_save();
    term_restore();
    exit(code);
  }

  if (!strcmp(n, "cd"))
    return builtin_cd(cmd);
  if (!strcmp(n, "pwd"))
    return builtin_pwd(cmd);
  if (!strcmp(n, "echo"))
    return builtin_echo(cmd);
  if (!strcmp(n, "export"))
    return builtin_export(cmd);
  if (!strcmp(n, "unset"))
    return builtin_unset(cmd);
  if (!strcmp(n, "env"))
    return builtin_env(cmd);
  if (!strcmp(n, "history"))
    return builtin_history(cmd);
  if (!strcmp(n, "alias"))
    return builtin_alias(cmd);
  if (!strcmp(n, "unalias"))
    return builtin_unalias(cmd);
  if (!strcmp(n, "type"))
    return builtin_type(cmd);
  if (!strcmp(n, "help"))
    return builtin_help(cmd);
  if (!strcmp(n, "true"))
    return 0;
  if (!strcmp(n, "false"))
    return 1;

  if (!strcmp(n, "source") || !strcmp(n, "."))
    return builtin_source(cmd);

  return -1; /* Not a built-in */
}