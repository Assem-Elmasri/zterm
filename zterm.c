/*
 * msh - Mini Shell
 * A functional Bash-like shell in C
 *
 * Features:
 *  - Command execution with argument parsing
 *  - Built-in commands: cd, pwd, echo, exit, help, history, export, unset, env,
 * alias
 *  - I/O redirection: >, >>, <, 2>, &>
 *  - Pipes: cmd1 | cmd2 | cmd3 ...
 *  - Background execution: cmd &
 *  - Environment variable expansion: $VAR, $?
 *  - Command history (up/down arrows via readline)
 *  - Signal handling: Ctrl+C (SIGINT), Ctrl+Z (SIGTSTP)
 *  - Wildcard/glob expansion: *.c, file?.txt
 *  - Tilde expansion: ~/path
 *  - Alias support
 *  - Logical operators: &&, ||
 *  - Command substitution: $(cmd)
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ─── Constants ─────────────────────────────────────────────── */
#define MAX_INPUT 4096
#define MAX_ARGS 256
#define MAX_HISTORY 1000
#define MAX_ALIASES 128
#define PIPE_READ 0
#define PIPE_WRITE 1
#define COLOR_RESET "\033[0m"
#define COLOR_BOLD "\033[1m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_CYAN "\033[36m"
#define COLOR_MAGENTA "\033[35m"

/* ─── Data Structures ────────────────────────────────────────── */

typedef struct {
  char *entries[MAX_HISTORY];
  int count;
  int pos; /* navigation cursor */
} History;

typedef struct {
  char *name;
  char *value;
} Alias;

typedef struct {
  char *input_file;
  char *output_file;
  char *error_file;
  int append;     /* >> vs > */
  int stderr_too; /* &> */
} Redirect;

typedef struct Command {
  char *args[MAX_ARGS];
  int argc;
  Redirect redir;
  int background;
} Command;

typedef struct Pipeline {
  Command cmds[MAX_ARGS];
  int count;
  int logic; /* 0=none, 1=&&, 2=|| */
} Pipeline;

/* ─── Globals ────────────────────────────────────────────────── */
static History g_history = {0};
static Alias g_aliases[MAX_ALIASES];
static int g_alias_count = 0;
static int g_last_status = 0;
static volatile sig_atomic_t g_child_pid = -1;
static struct termios g_orig_termios;
static int g_interactive = 0;

/* ─── Forward Declarations ───────────────────────────────────── */
int shell_execute(Pipeline *pl);
void print_prompt(void);
char *read_line(void);

/* ══════════════════════════════════════════════════════════════
   HISTORY
   ══════════════════════════════════════════════════════════════ */

void history_add(const char *line) {
  if (!line || !*line)
    return;
  /* avoid duplicate adjacent entries */
  if (g_history.count > 0 &&
      strcmp(g_history.entries[g_history.count - 1], line) == 0)
    return;
  if (g_history.count < MAX_HISTORY) {
    g_history.entries[g_history.count++] = strdup(line);
  } else {
    free(g_history.entries[0]);
    memmove(&g_history.entries[0], &g_history.entries[1],
            (MAX_HISTORY - 1) * sizeof(char *));
    g_history.entries[MAX_HISTORY - 1] = strdup(line);
  }
  g_history.pos = g_history.count;
}

void history_save(void) {
  char path[512];
  const char *home = getenv("HOME");
  if (!home)
    return;
  snprintf(path, sizeof(path), "%s/.msh_history", home);
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  for (int i = 0; i < g_history.count; i++)
    fprintf(f, "%s\n", g_history.entries[i]);
  fclose(f);
}

void history_load(void) {
  char path[512];
  const char *home = getenv("HOME");
  if (!home)
    return;
  snprintf(path, sizeof(path), "%s/.msh_history", home);
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char buf[MAX_INPUT];
  while (fgets(buf, sizeof(buf), f)) {
    buf[strcspn(buf, "\n")] = '\0';
    history_add(buf);
  }
  fclose(f);
}

/* ══════════════════════════════════════════════════════════════
   SIGNAL HANDLING
   ══════════════════════════════════════════════════════════════ */

void sigint_handler(int sig) {
  (void)sig;
  if (g_child_pid > 0) {
    kill(g_child_pid, SIGINT);
  } else {
    write(STDOUT_FILENO, "\n", 1);
    print_prompt();
    fflush(stdout);
  }
}

void sigchld_handler(int sig) {
  (void)sig;
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    if (WIFEXITED(status)) {
      /* background job finished */
    }
  }
}

void setup_signals(void) {
  struct sigaction sa_int = {0}, sa_chld = {0};
  sa_int.sa_handler = sigint_handler;
  sa_int.sa_flags = SA_RESTART;
  sigaction(SIGINT, &sa_int, NULL);

  sa_chld.sa_handler = sigchld_handler;
  sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa_chld, NULL);

  signal(SIGTTOU, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
}

/* ══════════════════════════════════════════════════════════════
   TERMINAL / LINE EDITOR
   ══════════════════════════════════════════════════════════════ */

#define KEY_UP 0x415b1b
#define KEY_DOWN 0x425b1b
#define KEY_LEFT 0x435b1b /* note: actually D for left */
#define KEY_RIGHT 0x435b1b

static char g_linebuf[MAX_INPUT];
static int g_linelen = 0;
static int g_cursor = 0;

void term_raw(void) {
  struct termios raw = g_orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void term_restore(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios); }

void line_refresh(void) {
  /* move to start of input area, reprint */
  write(STDOUT_FILENO, "\r\033[K", 4); /* CR + erase line */
  /* reprint prompt */
  /* NB: prompt already printed; just reprint current buffer */
  write(STDOUT_FILENO, g_linebuf, g_linelen);
  /* position cursor */
  if (g_cursor < g_linelen) {
    char seq[16];
    snprintf(seq, sizeof(seq), "\033[%dD", g_linelen - g_cursor);
    write(STDOUT_FILENO, seq, strlen(seq));
  }
}

/* ══════════════════════════════════════════════════════════════
   PROMPT
   ══════════════════════════════════════════════════════════════ */

void print_prompt(void) {
  char cwd[512];
  char host[64];
  const char *user = getenv("USER");
  if (!user)
    user = "user";
  gethostname(host, sizeof(host));
  if (!getcwd(cwd, sizeof(cwd)))
    strcpy(cwd, "?");

  /* shorten home dir to ~ */
  const char *home = getenv("HOME");
  char display_cwd[512];
  if (home && strncmp(cwd, home, strlen(home)) == 0) {
    snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
  } else {
    strncpy(display_cwd, cwd, sizeof(display_cwd));
  }

  /* color: green for normal user, red for root */
  const char *uc = (getuid() == 0) ? COLOR_RED : COLOR_GREEN;
  const char *symbol = (getuid() == 0) ? "#" : "$";

  printf("%s%s%s@%s%s%s:%s%s%s%s%s ", COLOR_BOLD, uc, user, host, COLOR_RESET,
         COLOR_BOLD, COLOR_BLUE, display_cwd, COLOR_RESET, COLOR_BOLD, symbol);
  fflush(stdout);
}

/* ══════════════════════════════════════════════════════════════
   LINE READER  (with arrow-key history + basic editing)
   ══════════════════════════════════════════════════════════════ */

char *read_line(void) {
  if (!g_interactive) {
    /* non-interactive: simple fgets */
    static char buf[MAX_INPUT];
    if (!fgets(buf, sizeof(buf), stdin))
      return NULL;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
  }

  memset(g_linebuf, 0, sizeof(g_linebuf));
  g_linelen = 0;
  g_cursor = 0;
  int hist_pos = g_history.count;

  term_raw();
  while (1) {
    unsigned char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) {
      term_restore();
      return NULL;
    }

    if (c == '\n' || c == '\r') {
      write(STDOUT_FILENO, "\n", 1);
      break;
    } else if (c == 4) { /* Ctrl-D */
      if (g_linelen == 0) {
        term_restore();
        return NULL;
      }
    } else if (c == 3) { /* Ctrl-C */
      write(STDOUT_FILENO, "^C\n", 3);
      term_restore();
      print_prompt();
      term_raw();
      g_linelen = 0;
      g_cursor = 0;
      memset(g_linebuf, 0, sizeof(g_linebuf));
      hist_pos = g_history.count;
      continue;
    } else if (c == 127 || c == 8) { /* Backspace */
      if (g_cursor > 0) {
        memmove(&g_linebuf[g_cursor - 1], &g_linebuf[g_cursor],
                g_linelen - g_cursor);
        g_cursor--;
        g_linelen--;
        g_linebuf[g_linelen] = '\0';
        line_refresh();
      }
    } else if (c == '\033') { /* escape sequence */
      unsigned char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) <= 0)
        continue;
      if (read(STDIN_FILENO, &seq[1], 1) <= 0)
        continue;
      if (seq[0] == '[') {
        if (seq[1] == 'A') { /* UP */
          if (hist_pos > 0) {
            hist_pos--;
            strncpy(g_linebuf, g_history.entries[hist_pos], MAX_INPUT - 1);
            g_linelen = strlen(g_linebuf);
            g_cursor = g_linelen;
            /* reprint full line */
            write(STDOUT_FILENO, "\r\033[K", 4);
            print_prompt();
            write(STDOUT_FILENO, g_linebuf, g_linelen);
          }
        } else if (seq[1] == 'B') { /* DOWN */
          if (hist_pos < g_history.count) {
            hist_pos++;
            if (hist_pos == g_history.count) {
              g_linebuf[0] = '\0';
              g_linelen = 0;
              g_cursor = 0;
            } else {
              strncpy(g_linebuf, g_history.entries[hist_pos], MAX_INPUT - 1);
              g_linelen = strlen(g_linebuf);
              g_cursor = g_linelen;
            }
            write(STDOUT_FILENO, "\r\033[K", 4);
            print_prompt();
            write(STDOUT_FILENO, g_linebuf, g_linelen);
          }
        } else if (seq[1] == 'C') { /* RIGHT */
          if (g_cursor < g_linelen) {
            g_cursor++;
            write(STDOUT_FILENO, "\033[C", 3);
          }
        } else if (seq[1] == 'D') { /* LEFT */
          if (g_cursor > 0) {
            g_cursor--;
            write(STDOUT_FILENO, "\033[D", 3);
          }
        } else if (seq[1] == 'H') { /* Home */
          g_cursor = 0;
          line_refresh();
        } else if (seq[1] == 'F') { /* End */
          g_cursor = g_linelen;
          line_refresh();
        } else if (seq[1] == '3') { /* Delete key: ESC[3~ */
          unsigned char tilde;
          read(STDIN_FILENO, &tilde, 1);
          if (tilde == '~' && g_cursor < g_linelen) {
            memmove(&g_linebuf[g_cursor], &g_linebuf[g_cursor + 1],
                    g_linelen - g_cursor - 1);
            g_linelen--;
            g_linebuf[g_linelen] = '\0';
            line_refresh();
          }
        }
      }
    } else if (c == 1) { /* Ctrl-A: beginning of line */
      g_cursor = 0;
      line_refresh();
    } else if (c == 5) { /* Ctrl-E: end of line */
      g_cursor = g_linelen;
      line_refresh();
    } else if (c == 11) { /* Ctrl-K: kill to end */
      g_linelen = g_cursor;
      g_linebuf[g_linelen] = '\0';
      line_refresh();
    } else if (c == 21) { /* Ctrl-U: kill to start */
      memmove(g_linebuf, &g_linebuf[g_cursor], g_linelen - g_cursor);
      g_linelen -= g_cursor;
      g_cursor = 0;
      g_linebuf[g_linelen] = '\0';
      line_refresh();
    } else if (isprint(c)) {
      if (g_linelen < MAX_INPUT - 1) {
        memmove(&g_linebuf[g_cursor + 1], &g_linebuf[g_cursor],
                g_linelen - g_cursor);
        g_linebuf[g_cursor++] = c;
        g_linelen++;
        g_linebuf[g_linelen] = '\0';
        line_refresh();
      }
    }
  }
  term_restore();
  return g_linebuf;
}

/* ══════════════════════════════════════════════════════════════
   EXPANSION  (variables, tilde, globs, command substitution)
   ══════════════════════════════════════════════════════════════ */

/* Expand $VAR and ${VAR} and $? in-place, returns new heap string */
char *expand_vars(const char *s) {
  char out[MAX_INPUT * 4];
  int oi = 0;
  int si = 0;
  int len = strlen(s);

  while (si < len && oi < (int)sizeof(out) - 1) {
    if (s[si] == '\\' && si + 1 < len) {
      out[oi++] = s[++si];
      si++;
      continue;
    }
    if (s[si] == '$') {
      si++;
      if (s[si] == '?') {
        /* exit status */
        char num[16];
        snprintf(num, sizeof(num), "%d", g_last_status);
        int nl = strlen(num);
        if (oi + nl < (int)sizeof(out) - 1) {
          memcpy(&out[oi], num, nl);
          oi += nl;
        }
        si++;
      } else if (s[si] == '{') {
        si++;
        char var[256];
        int vi = 0;
        while (s[si] && s[si] != '}' && vi < 255)
          var[vi++] = s[si++];
        var[vi] = '\0';
        if (s[si] == '}')
          si++;
        const char *v = getenv(var);
        if (v) {
          int vl = strlen(v);
          if (oi + vl < (int)sizeof(out) - 1) {
            memcpy(&out[oi], v, vl);
            oi += vl;
          }
        }
      } else if (isalpha((unsigned char)s[si]) || s[si] == '_') {
        char var[256];
        int vi = 0;
        while ((isalnum((unsigned char)s[si]) || s[si] == '_') && vi < 255)
          var[vi++] = s[si++];
        var[vi] = '\0';
        const char *v = getenv(var);
        if (v) {
          int vl = strlen(v);
          if (oi + vl < (int)sizeof(out) - 1) {
            memcpy(&out[oi], v, vl);
            oi += vl;
          }
        }
      } else {
        out[oi++] = '$';
      }
    } else {
      out[oi++] = s[si++];
    }
  }
  out[oi] = '\0';
  return strdup(out);
}

/* Expand leading ~ */
char *expand_tilde(const char *s) {
  if (s[0] != '~')
    return strdup(s);
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    home = pw ? pw->pw_dir : "/";
  }
  char out[MAX_INPUT];
  if (s[1] == '/' || s[1] == '\0') {
    snprintf(out, sizeof(out), "%s%s", home, s + 1);
  } else {
    /* ~username */
    char uname[64];
    int i = 0;
    const char *p = s + 1;
    while (*p && *p != '/' && i < 63)
      uname[i++] = *p++;
    uname[i] = '\0';
    struct passwd *pw = getpwnam(uname);
    if (pw)
      snprintf(out, sizeof(out), "%s%s", pw->pw_dir, p);
    else
      strncpy(out, s, sizeof(out));
  }
  return strdup(out);
}

/* Command substitution: replace $(cmd) with its output */
char *expand_cmdsub(const char *s); /* forward */

/* Full word expansion: tilde + vars + cmdsub */
char *expand_word(const char *w) {
  char *t = expand_tilde(w);
  char *v = expand_vars(t);
  free(t);
  char *c = expand_cmdsub(v);
  free(v);
  return c;
}

/* ══════════════════════════════════════════════════════════════
   TOKENIZER / PARSER
   ══════════════════════════════════════════════════════════════ */

typedef enum {
  TOK_WORD,
  TOK_PIPE,
  TOK_REDIR_IN,
  TOK_REDIR_OUT,
  TOK_REDIR_APPEND,
  TOK_REDIR_ERR,
  TOK_REDIR_BOTH,
  TOK_BACKGROUND,
  TOK_AND,
  TOK_OR,
  TOK_SEMICOLON,
  TOK_EOF
} TokenType;

typedef struct {
  TokenType type;
  char value[MAX_INPUT];
} Token;

typedef struct {
  const char *src;
  int pos;
  Token cur;
} Lexer;

/* Read one raw token from the input */
Token lexer_next(Lexer *l) {
  Token tok = {0};
  const char *s = l->src;
  int i = l->pos;

  /* skip whitespace */
  while (s[i] == ' ' || s[i] == '\t')
    i++;

  if (!s[i]) {
    tok.type = TOK_EOF;
    l->pos = i;
    return tok;
  }

  /* comment */
  if (s[i] == '#') {
    tok.type = TOK_EOF;
    l->pos = strlen(s);
    return tok;
  }

  /* operators */
  if (s[i] == '|' && s[i + 1] == '|') {
    tok.type = TOK_OR;
    i += 2;
  } else if (s[i] == '&' && s[i + 1] == '&') {
    tok.type = TOK_AND;
    i += 2;
  } else if (s[i] == '>' && s[i + 1] == '>') {
    tok.type = TOK_REDIR_APPEND;
    i += 2;
  } else if (s[i] == '2' && s[i + 1] == '>') {
    tok.type = TOK_REDIR_ERR;
    i += 2;
  } else if (s[i] == '&' && s[i + 1] == '>') {
    tok.type = TOK_REDIR_BOTH;
    i += 2;
  } else if (s[i] == '|') {
    tok.type = TOK_PIPE;
    i++;
  } else if (s[i] == '>') {
    tok.type = TOK_REDIR_OUT;
    i++;
  } else if (s[i] == '<') {
    tok.type = TOK_REDIR_IN;
    i++;
  } else if (s[i] == '&') {
    tok.type = TOK_BACKGROUND;
    i++;
  } else if (s[i] == ';') {
    tok.type = TOK_SEMICOLON;
    i++;
  } else {
    /* word: handle quoting */
    tok.type = TOK_WORD;
    int wi = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '|' && s[i] != '>' &&
           s[i] != '<' && s[i] != '&' && s[i] != ';' && s[i] != '#' &&
           wi < MAX_INPUT - 1) {
      if (s[i] == '"') {
        i++;
        while (s[i] && s[i] != '"' && wi < MAX_INPUT - 1) {
          if (s[i] == '\\' && s[i + 1]) {
            i++;
            tok.value[wi++] = s[i++];
          } else {
            tok.value[wi++] = s[i++];
          }
        }
        if (s[i] == '"')
          i++;
      } else if (s[i] == '\'') {
        i++;
        while (s[i] && s[i] != '\'' && wi < MAX_INPUT - 1)
          tok.value[wi++] = s[i++];
        if (s[i] == '\'')
          i++;
      } else if (s[i] == '\\' && s[i + 1]) {
        i++;
        tok.value[wi++] = s[i++];
      } else {
        tok.value[wi++] = s[i++];
      }
    }
    tok.value[wi] = '\0';
  }

  l->pos = i;
  return tok;
}

/* Parse a single command (no pipes/logic) */
int parse_command(Lexer *l, Command *cmd) {
  memset(cmd, 0, sizeof(*cmd));
  Token tok;
  while (1) {
    tok = lexer_next(l);
    if (tok.type == TOK_EOF || tok.type == TOK_PIPE || tok.type == TOK_AND ||
        tok.type == TOK_OR || tok.type == TOK_SEMICOLON) {
      /* push back conceptually — store position */
      /* We just break; caller re-reads via the token type */
      break;
    }
    if (tok.type == TOK_BACKGROUND) {
      cmd->background = 1;
      break;
    }
    if (tok.type == TOK_REDIR_IN) {
      Token fn = lexer_next(l);
      cmd->redir.input_file = strdup(fn.value);
    } else if (tok.type == TOK_REDIR_OUT) {
      Token fn = lexer_next(l);
      cmd->redir.output_file = strdup(fn.value);
      cmd->redir.append = 0;
    } else if (tok.type == TOK_REDIR_APPEND) {
      Token fn = lexer_next(l);
      cmd->redir.output_file = strdup(fn.value);
      cmd->redir.append = 1;
    } else if (tok.type == TOK_REDIR_ERR) {
      Token fn = lexer_next(l);
      cmd->redir.error_file = strdup(fn.value);
    } else if (tok.type == TOK_REDIR_BOTH) {
      Token fn = lexer_next(l);
      cmd->redir.output_file = strdup(fn.value);
      cmd->redir.error_file = strdup(fn.value);
      cmd->redir.stderr_too = 1;
    } else {
      /* word — expand and possibly glob */
      char *expanded = expand_word(tok.value);
      /* glob expansion */
      if (strpbrk(expanded, "*?[")) {
        glob_t gb;
        if (glob(expanded, GLOB_NOCHECK | GLOB_TILDE, NULL, &gb) == 0) {
          for (size_t g = 0; g < gb.gl_pathc && cmd->argc < MAX_ARGS - 1; g++)
            cmd->args[cmd->argc++] = strdup(gb.gl_pathv[g]);
          globfree(&gb);
        } else {
          cmd->args[cmd->argc++] = expanded;
          expanded = NULL;
        }
      } else {
        cmd->args[cmd->argc++] = expanded;
        expanded = NULL;
      }
      free(expanded);
    }
  }
  cmd->args[cmd->argc] = NULL;
  return tok.type; /* return delimiter type */
}

/* Parse a full pipeline (with optional && / || / ; ) */
int parse_pipeline(const char *line, Pipeline *pl) {
  memset(pl, 0, sizeof(*pl));
  Lexer l = {.src = line, .pos = 0};

  while (1) {
    if (pl->count >= MAX_ARGS)
      break;
    int delim = parse_command(&l, &pl->cmds[pl->count]);
    if (pl->cmds[pl->count].argc == 0 && !pl->cmds[pl->count].redir.input_file)
      break; /* empty command */
    pl->count++;
    if (delim == TOK_EOF || delim == TOK_SEMICOLON)
      break;
    if (delim == TOK_AND || delim == TOK_OR)
      break;
    if (delim != TOK_PIPE)
      break;
  }
  return pl->count;
}

/* ══════════════════════════════════════════════════════════════
   COMMAND SUBSTITUTION  $(...)
   ══════════════════════════════════════════════════════════════ */

char *expand_cmdsub(const char *s) {
  char out[MAX_INPUT * 4];
  int oi = 0, si = 0;
  int len = strlen(s);

  while (si < len && oi < (int)sizeof(out) - 2) {
    if (s[si] == '$' && s[si + 1] == '(') {
      /* find matching ) */
      int depth = 1, start = si + 2;
      si += 2;
      while (s[si] && depth > 0) {
        if (s[si] == '(')
          depth++;
        else if (s[si] == ')')
          depth--;
        si++;
      }
      /* s[start .. si-1) is the inner command */
      char inner[MAX_INPUT];
      int ilen = si - start - 1;
      if (ilen < 0)
        ilen = 0;
      if (ilen >= MAX_INPUT)
        ilen = MAX_INPUT - 1;
      strncpy(inner, s + start, ilen);
      inner[ilen] = '\0';

      /* run inner command and capture stdout */
      int pfd[2];
      if (pipe(pfd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
          close(pfd[PIPE_READ]);
          dup2(pfd[PIPE_WRITE], STDOUT_FILENO);
          close(pfd[PIPE_WRITE]);
          /* execute inner command */
          Pipeline ipl;
          parse_pipeline(inner, &ipl);
          shell_execute(&ipl);
          exit(g_last_status);
        }
        close(pfd[PIPE_WRITE]);
        char buf[MAX_INPUT];
        int nr;
        while ((nr = read(pfd[PIPE_READ], buf, sizeof(buf) - 1)) > 0) {
          buf[nr] = '\0';
          /* strip trailing newlines */
          while (nr > 0 && buf[nr - 1] == '\n')
            buf[--nr] = '\0';
          if (oi + nr < (int)sizeof(out) - 1) {
            memcpy(&out[oi], buf, nr);
            oi += nr;
          }
        }
        close(pfd[PIPE_READ]);
        waitpid(pid, NULL, 0);
      }
    } else {
      out[oi++] = s[si++];
    }
  }
  out[oi] = '\0';
  return strdup(out);
}

/* ══════════════════════════════════════════════════════════════
   ALIAS MANAGEMENT
   ══════════════════════════════════════════════════════════════ */

Alias *alias_find(const char *name) {
  for (int i = 0; i < g_alias_count; i++)
    if (strcmp(g_aliases[i].name, name) == 0)
      return &g_aliases[i];
  return NULL;
}

/* Expand aliases in the first word of a command */
void alias_expand(Command *cmd) {
  if (cmd->argc == 0)
    return;
  Alias *a = alias_find(cmd->args[0]);
  if (!a)
    return;
  /* re-parse the alias value + remaining args */
  char newline[MAX_INPUT];
  snprintf(newline, sizeof(newline), "%s", a->value);
  for (int i = 1; i < cmd->argc; i++) {
    strncat(newline, " ", sizeof(newline) - strlen(newline) - 1);
    strncat(newline, cmd->args[i], sizeof(newline) - strlen(newline) - 1);
  }
  /* free old args */
  for (int i = 0; i < cmd->argc; i++) {
    free(cmd->args[i]);
    cmd->args[i] = NULL;
  }
  cmd->argc = 0;

  Pipeline tmp;
  parse_pipeline(newline, &tmp);
  if (tmp.count > 0) {
    Command *tc = &tmp.cmds[0];
    for (int i = 0; i < tc->argc; i++)
      cmd->args[cmd->argc++] = strdup(tc->args[i]);
    cmd->args[cmd->argc] = NULL;
  }
}

/* ══════════════════════════════════════════════════════════════
   BUILT-IN COMMANDS
   ══════════════════════════════════════════════════════════════ */

int builtin_cd(Command *cmd) {
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

int builtin_pwd(Command *cmd) {
  (void)cmd;
  char cwd[512];
  if (getcwd(cwd, sizeof(cwd)))
    printf("%s\n", cwd);
  else {
    perror("pwd");
    return 1;
  }
  return 0;
}

int builtin_echo(Command *cmd) {
  int newline = 1;
  int start = 1;
  if (cmd->argc > 1 && strcmp(cmd->args[1], "-n") == 0) {
    newline = 0;
    start = 2;
  }
  int interpret = 0;
  if (cmd->argc > 1 && strcmp(cmd->args[start], "-e") == 0) {
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

int builtin_export(Command *cmd) {
  if (cmd->argc == 1) {
    /* print all exported vars */
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
      /* just mark existing local var as exported */
      const char *v = getenv(cmd->args[i]);
      if (v)
        setenv(cmd->args[i], v, 1);
    }
  }
  return 0;
}

int builtin_unset(Command *cmd) {
  for (int i = 1; i < cmd->argc; i++)
    unsetenv(cmd->args[i]);
  return 0;
}

int builtin_env(Command *cmd) {
  (void)cmd;
  extern char **environ;
  for (char **e = environ; *e; e++)
    printf("%s\n", *e);
  return 0;
}

int builtin_history(Command *cmd) {
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

int builtin_alias(Command *cmd) {
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
    /* strip surrounding quotes */
    if ((val[0] == '\'' || val[0] == '"') && val[strlen(val) - 1] == val[0]) {
      val[strlen(val) - 1] = '\0';
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

int builtin_unalias(Command *cmd) {
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

int builtin_type(Command *cmd) {
  for (int i = 1; i < cmd->argc; i++) {
    const char *name = cmd->args[i];
    if (alias_find(name)) {
      Alias *a = alias_find(name);
      printf("%s is aliased to '%s'\n", name, a->value);
      continue;
    }
    /* check PATH */
    const char *path = getenv("PATH");
    if (!path)
      path = "/usr/local/bin:/usr/bin:/bin";
    char pathcopy[MAX_INPUT];
    strncpy(pathcopy, path, sizeof(pathcopy));
    char *dir = strtok(pathcopy, ":");
    int found = 0;
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

int builtin_help(Command *cmd) {
  (void)cmd;
  printf("\n%s%smsh%s — Mini Shell  (inspired by bash)\n\n", COLOR_BOLD,
         COLOR_CYAN, COLOR_RESET);
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

/* ══════════════════════════════════════════════════════════════
   I/O REDIRECTION SETUP  (called in child)
   ══════════════════════════════════════════════════════════════ */

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

/* ══════════════════════════════════════════════════════════════
   EXECUTE A SINGLE COMMAND  (builtin or external)
   ══════════════════════════════════════════════════════════════ */

/* Returns 0 if handled as builtin, -1 if not a builtin */
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
  if (!strcmp(n, "source") || !strcmp(n, ".")) {
    /* source a file */
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
  return -1; /* not a builtin */
}

/* Fork + exec one command in a pipeline */
pid_t exec_command(Command *cmd, int in_fd, int out_fd) {
  alias_expand(cmd);
  if (cmd->argc == 0)
    return -1;

  /* handle builtins in-process when no pipes */
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
    /* child */
    if (in_fd != STDIN_FILENO) {
      dup2(in_fd, STDIN_FILENO);
      close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
      dup2(out_fd, STDOUT_FILENO);
      close(out_fd);
    }

    /* restore default signal handlers */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    if (apply_redirections(&cmd->redir) < 0)
      exit(1);

    /* builtins with redirected IO */
    int r = try_builtin(cmd);
    if (r >= 0)
      exit(r);

    execvp(cmd->args[0], cmd->args);
    fprintf(stderr, "msh: %s: %s\n", cmd->args[0], strerror(errno));
    exit(127);
  }
  return pid;
}

/* ══════════════════════════════════════════════════════════════
   PIPELINE EXECUTION
   ══════════════════════════════════════════════════════════════ */

int shell_execute(Pipeline *pl) {
  if (pl->count == 0)
    return 0;
  int status = 0;

  /* single command special case */
  if (pl->count == 1) {
    Command *cmd = &pl->cmds[0];
    alias_expand(cmd);

    /* apply redir, try builtin in-process */
    int r = try_builtin(cmd);
    if (r >= 0) {
      /* handle redirections for builtins by wrapping in fork */
      /* only fork if there are actual redirections */
      int has_redir = cmd->redir.input_file || cmd->redir.output_file ||
                      cmd->redir.error_file;
      if (has_redir) {
        pid_t pid = fork();
        if (pid == 0) {
          signal(SIGINT, SIG_DFL);
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

    /* external command */
    pid_t pid = fork();
    if (pid == 0) {
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);
      signal(SIGCHLD, SIG_DFL);
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

  /* multi-command pipeline */
  int pipes[MAX_ARGS][2];
  pid_t pids[MAX_ARGS];
  int npipes = pl->count - 1;

  for (int i = 0; i < npipes; i++)
    pipe(pipes[i]);

  for (int i = 0; i < pl->count; i++) {
    int in_fd = (i == 0) ? STDIN_FILENO : pipes[i - 1][PIPE_READ];
    int out_fd = (i == pl->count - 1) ? STDOUT_FILENO : pipes[i][PIPE_WRITE];

    pid_t pid = fork();
    if (pid == 0) {
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);
      signal(SIGCHLD, SIG_DFL);

      if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
      }
      if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
      }

      /* close all pipe fds in child */
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

  /* close all pipe ends in parent */
  for (int i = 0; i < npipes; i++) {
    close(pipes[i][PIPE_READ]);
    close(pipes[i][PIPE_WRITE]);
  }

  /* wait for all */
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

/* ══════════════════════════════════════════════════════════════
   MULTI-PIPELINE  (handle &&, ||, ;)
   ══════════════════════════════════════════════════════════════ */

int execute_line(const char *line) {
  /* split on ; first, then handle && / || inside each segment */
  /* simple approach: run parse + check logical ops token by token */

  char segment[MAX_INPUT];
  int si = 0;
  const char *p = line;
  int status = 0;

  /* We re-lex to find top-level ; && || splits */
  while (*p) {
    /* collect next segment */
    si = 0;
    int in_quote = 0;
    char qc = 0;
    while (*p) {
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
            /* skip until next ;  */
            while (*p && !(*p == ';'))
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
            /* skip rest of || chain */
            while (*p && !(*p == ';'))
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

/* ══════════════════════════════════════════════════════════════
   RC FILE
   ══════════════════════════════════════════════════════════════ */

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

/* ══════════════════════════════════════════════════════════════
   MAIN LOOP
   ══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
  /* check if interactive */
  g_interactive = isatty(STDIN_FILENO);

  /* save terminal state */
  if (g_interactive)
    tcgetattr(STDIN_FILENO, &g_orig_termios);

  setup_signals();

  /* default env */
  if (!getenv("HOME")) {
    struct passwd *pw = getpwuid(getuid());
    if (pw)
      setenv("HOME", pw->pw_dir, 1);
  }
  if (!getenv("PATH"))
    setenv("PATH",
           "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin", 1);
  if (!getenv("USER")) {
    struct passwd *pw = getpwuid(getuid());
    if (pw)
      setenv("USER", pw->pw_name, 1);
  }

  /* set shell variable */
  setenv("SHELL", "msh", 1);

  history_load();
  load_rc();

  /* ── non-interactive / script mode ── */
  if (argc > 1) {
    if (strcmp(argv[1], "-c") == 0 && argc > 2) {
      /* -c "command" */
      g_interactive = 0;
      int r = execute_line(argv[2]);
      history_save();
      return r;
    }
    /* script file */
    FILE *f = fopen(argv[1], "r");
    if (!f) {
      fprintf(stderr, "msh: %s: %s\n", argv[1], strerror(errno));
      return 1;
    }
    g_interactive = 0;
    char buf[MAX_INPUT];
    while (fgets(buf, sizeof(buf), f)) {
      buf[strcspn(buf, "\n")] = '\0';
      if (!buf[0] || buf[0] == '#')
        continue;
      /* handle shebang */
      if (buf[0] == '#' && buf[1] == '!')
        continue;
      execute_line(buf);
    }
    fclose(f);
    history_save();
    return g_last_status;
  }

  /* ── interactive mode ── */
  if (g_interactive) {
    printf("%smsh%s v1.0  —  type %shelp%s for commands\n",
           COLOR_BOLD COLOR_CYAN, COLOR_RESET, COLOR_BOLD, COLOR_RESET);
  }

  while (1) {
    if (g_interactive)
      print_prompt();
    char *line = read_line();
    if (!line) {
      if (g_interactive)
        printf("\nexit\n");
      break;
    }

    /* trim */
    while (*line == ' ' || *line == '\t')
      line++;
    if (!*line)
      continue;

    history_add(line);
    execute_line(line);
  }

  history_save();
  if (g_interactive)
    term_restore();
  return g_last_status;
}