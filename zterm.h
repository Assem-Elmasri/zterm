
#ifndef ZTERM_H
#define ZTERM_H

#define _GNU_SOURCE

// headers
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

// limits
// linux/limits.h also defines MAX_INPUT (255); override it intentionally
#ifdef MAX_INPUT
#undef MAX_INPUT
#endif
#define MAX_INPUT 4096
#define MAX_ARGS 256
#define MAX_HISTORY 1000
#define MAX_ALIASES 128
#define PIPE_READ 0
#define PIPE_WRITE 1

// ANSI color codes
#define COLOR_RESET "\033[0m"
#define COLOR_BOLD "\033[1m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_CYAN "\033[36m"
#define COLOR_MAGENTA "\033[35m"

// DS

typedef struct {
  char *entries[MAX_HISTORY];
  int count;
  int pos;
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
  int stderr_too; /* &>      */
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
  int logic; // 0=none  1=&&  2=||
} Pipeline;

// global states
extern History g_history;
extern Alias g_aliases[MAX_ALIASES];
extern int g_alias_count;
extern int g_last_status;
extern volatile sig_atomic_t g_child_pid;
extern struct termios g_orig_termios;
extern int g_interactive;

// history.c
void history_add(const char *line);
void history_save(void);
void history_load(void);

// signals.c
void setup_signals(void);

// terminal.c
void term_raw(void);
void term_restore(void);
void print_prompt(void);
char *read_line(void);

// expand.c
char *expand_vars(const char *s);
char *expand_tilde(const char *s);
char *expand_cmdsub(const char *s);
char *expand_word(const char *w);

// parser.c
int parse_pipeline(const char *line, Pipeline *pl);

// alias.c
Alias *alias_find(const char *name);
void alias_expand(Command *cmd);

// builtins.c
int try_builtin(Command *cmd);

// exec.c
int apply_redirections(Redirect *r);
pid_t exec_command(Command *cmd, int in_fd, int out_fd);
int shell_execute(Pipeline *pl);
int execute_line(const char *line);
void load_rc(void);

#endif
