/*
 * exec.c — Command and pipeline execution.
 *
 *  apply_redirections()  Set up stdin/stdout/stderr in a child process.
 *  exec_command()        Fork + exec a single command (or run a built-in
 *                        in-process when no pipe is involved).
 *  shell_execute()       Execute a parsed Pipeline, handling pipes and
 *                        background jobs.
 *  execute_line()        Split a raw input line on ';', '&&', '||' and
 *                        dispatch each segment to shell_execute().
 *  load_rc()             Source ~/.mshrc at startup.
 */

#include "zterm.h"

/* ── Globals defined here ──────────────────────────────────────────────── */
int g_last_status = 0;
volatile sig_atomic_t g_child_pid = -1;

/* ══════════════════════════════════════════════════════════════════════════
   I/O Redirection  (runs inside the child process)
   ══════════════════════════════════════════════════════════════════════════ */

int apply_redirections(Redirect *r) {
  if (r->input_file) {
    char *ef = expand_word(r->input_file);
    int fd = open(ef, O_RDONLY);
    free(ef);
    if (fd < 0) {
      perror(r->input_file);
      return -1;
    }
    dup2(fd, STDIN_FILENO);
    close(fd);
  }

  if (r->output_file) {
    char *ef = expand_word(r->output_file);
    int flags = O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC);
    int fd = open(ef, flags, 0644);
    free(ef);
    if (fd < 0) {
      perror(r->output_file);
      return -1;
    }
    dup2(fd, STDOUT_FILENO);
    if (r->stderr_too)
      dup2(fd, STDERR_FILENO);
    close(fd);
  }

  if (r->error_file && !r->stderr_too) {
    char *ef = expand_word(r->error_file);
    int fd = open(ef, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    free(ef);
    if (fd < 0) {
      perror(r->error_file);
      return -1;
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
  }

  return 0;
}

/* ── Restore default signal handlers in a child ────────────────────────── */
static void child_reset_signals(void) {
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);
}

/* ══════════════════════════════════════════════════════════════════════════
   Single-command executor
   ══════════════════════════════════════════════════════════════════════════ */

pid_t exec_command(Command *cmd, int in_fd, int out_fd) {
  alias_expand(cmd);
  if (cmd->argc == 0)
    return -1;

  /* Run built-ins in-process when not in a pipeline */
  if (in_fd == STDIN_FILENO && out_fd == STDOUT_FILENO) {
    int r = try_builtin(cmd);
    if (r >= 0) {
      g_last_status = r;
      return 0;
    }
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }

  if (pid == 0) {
    /* Child */
    if (in_fd != STDIN_FILENO) {
      dup2(in_fd, STDIN_FILENO);
      close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
      dup2(out_fd, STDOUT_FILENO);
      close(out_fd);
    }

    child_reset_signals();

    if (apply_redirections(&cmd->redir) < 0)
      exit(1);

    /* Built-ins with redirected I/O still run in a fork */
    int r = try_builtin(cmd);
    if (r >= 0)
      exit(r);

    execvp(cmd->args[0], cmd->args);
    fprintf(stderr, "msh: %s: %s\n", cmd->args[0], strerror(errno));
    exit(127);
  }

  return pid;
}

/* ══════════════════════════════════════════════════════════════════════════
   Pipeline execution
   ══════════════════════════════════════════════════════════════════════════ */

int shell_execute(Pipeline *pl) {
  if (pl->count == 0)
    return 0;

  int status = 0;

  /* ── Single command ─────────────────────────────────────────────────── */
  if (pl->count == 1) {
    Command *cmd = &pl->cmds[0];
    alias_expand(cmd);

    /* Try built-in in-process first */
    int r = try_builtin(cmd);
    if (r >= 0) {
      /* If there are redirections, re-run inside a fork */
      int has_redir = cmd->redir.input_file || cmd->redir.output_file ||
                      cmd->redir.error_file;
      if (has_redir) {
        pid_t pid = fork();
        if (pid == 0) {
          child_reset_signals();
          apply_redirections(&cmd->redir);
          exit(try_builtin(cmd));
        }
        int ws;
        waitpid(pid, &ws, 0);
        status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 1;
      } else {
        status = r;
      }
      goto done;
    }

    /* External command */
    pid_t pid = fork();
    if (pid == 0) {
      child_reset_signals();
      apply_redirections(&cmd->redir);
      execvp(cmd->args[0], cmd->args);
      fprintf(stderr, "msh: %s: %s\n", cmd->args[0], strerror(errno));
      exit(127);
    }
    if (pid < 0) {
      perror("fork");
      return 1;
    }

    g_child_pid = pid;
    if (cmd->background) {
      printf("[bg] %d\n", pid);
      status = 0;
    } else {
      int ws;
      waitpid(pid, &ws, 0);
      status = WIFEXITED(ws) ? WEXITSTATUS(ws) : (128 + WTERMSIG(ws));
    }
    g_child_pid = -1;
    goto done;
  }

  /* ── Multi-command pipeline ─────────────────────────────────────────── */
  int pipes[MAX_ARGS][2];
  pid_t pids[MAX_ARGS];
  int npipes = pl->count - 1;
  memset(pids, 0, sizeof(pids));

  for (int i = 0; i < npipes; i++)
    pipe(pipes[i]);

  for (int i = 0; i < pl->count; i++) {
    int in_fd = (i == 0) ? STDIN_FILENO : pipes[i - 1][PIPE_READ];
    int out_fd = (i == pl->count - 1) ? STDOUT_FILENO : pipes[i][PIPE_WRITE];

    pid_t pid = fork();
    if (pid == 0) {
      child_reset_signals();

      if (in_fd != STDIN_FILENO)
        dup2(in_fd, STDIN_FILENO);
      if (out_fd != STDOUT_FILENO)
        dup2(out_fd, STDOUT_FILENO);

      /* Close all pipe ends in child */
      for (int j = 0; j < npipes; j++) {
        close(pipes[j][PIPE_READ]);
        close(pipes[j][PIPE_WRITE]);
      }

      apply_redirections(&pl->cmds[i].redir);

      int r = try_builtin(&pl->cmds[i]);
      if (r >= 0)
        exit(r);

      execvp(pl->cmds[i].args[0], pl->cmds[i].args);
      fprintf(stderr, "msh: %s: %s\n", pl->cmds[i].args[0], strerror(errno));
      exit(127);
    }
    pids[i] = pid;
  }

  /* Close all pipe ends in parent */
  for (int i = 0; i < npipes; i++) {
    close(pipes[i][PIPE_READ]);
    close(pipes[i][PIPE_WRITE]);
  }

  /* Wait for all children; collect exit status of last command */
  int bg = pl->cmds[pl->count - 1].background;
  if (bg) {
    printf("[bg] %d\n", pids[pl->count - 1]);
  } else {
    g_child_pid = pids[pl->count - 1];
    for (int i = 0; i < pl->count; i++) {
      int ws;
      waitpid(pids[i], &ws, 0);
      if (i == pl->count - 1)
        status = WIFEXITED(ws) ? WEXITSTATUS(ws) : (128 + WTERMSIG(ws));
    }
    g_child_pid = -1;
  }

done:
  g_last_status = status;
  return status;
}

/* ══════════════════════════════════════════════════════════════════════════
   Line dispatcher  (handles ';', '&&', '||' at the top level)
   ══════════════════════════════════════════════════════════════════════════ */

int execute_line(const char *line) {
  char segment[MAX_INPUT];
  int si = 0;
  const char *p = line;
  int status = 0;

  while (*p) {
    si = 0;

    /* Collect next segment, watching for top-level ; && || */
    int in_quote = 0;
    char qc = 0;

    while (*p) {
      /* Track quoting to ignore operators inside strings */
      if (!in_quote && (*p == '\'' || *p == '"')) {
        in_quote = 1;
        qc = *p;
        segment[si++] = *p++;
        continue;
      }
      if (in_quote && *p == qc) {
        in_quote = 0;
        segment[si++] = *p++;
        continue;
      }

      if (!in_quote) {
        if (*p == ';') {
          p++;
          break;
        }

        if (*p == '&' && *(p + 1) == '&') {
          segment[si] = '\0';
          Pipeline pl;
          parse_pipeline(segment, &pl);
          status = shell_execute(&pl);
          if (status != 0) {
            /* Short-circuit: skip rest of this && chain */
            while (*p && *p != ';')
              p++;
            if (*p == ';')
              p++;
            goto next_segment;
          }
          p += 2;
          si = 0;
          continue;
        }

        if (*p == '|' && *(p + 1) == '|') {
          segment[si] = '\0';
          Pipeline pl;
          parse_pipeline(segment, &pl);
          status = shell_execute(&pl);
          if (status == 0) {
            /* Short-circuit: skip rest of this || chain */
            while (*p && *p != ';')
              p++;
            if (*p == ';')
              p++;
            goto next_segment;
          }
          p += 2;
          si = 0;
          continue;
        }
      }

      segment[si++] = *p++;
    }

    segment[si] = '\0';
    if (si > 0) {
      Pipeline pl;
      parse_pipeline(segment, &pl);
      status = shell_execute(&pl);
    }

  next_segment:;
  }

  return status;
}

/* ══════════════════════════════════════════════════════════════════════════
   RC file loader
   ══════════════════════════════════════════════════════════════════════════ */

void load_rc(void) {
  const char *home = getenv("HOME");
  if (!home)
    return;

  char path[512];
  snprintf(path, sizeof(path), "%s/.mshrc", home);

  FILE *f = fopen(path, "r");
  if (!f)
    return;

  char buf[MAX_INPUT];
  while (fgets(buf, sizeof(buf), f)) {
    buf[strcspn(buf, "\n")] = '\0';
    if (!buf[0] || buf[0] == '#')
      continue;
    execute_line(buf);
  }

  fclose(f);
}