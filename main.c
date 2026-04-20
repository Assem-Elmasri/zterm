// shell entry point
//  handles:
//  >> REPL
//  >> file exec
//  >> command exec

#include "zterm.h"

// sets SHELL env variable and make sure others is set
static void bootstrap_env(void) {
  struct passwd *pw;

  if (!getenv("HOME")) {
    pw = getpwuid(getuid()); // gets user data
    if (pw)
      setenv("HOME", pw->pw_dir, 1);
  }

  if (!getenv("PATH"))
    setenv("PATH",
           "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin", 1);

  if (!getenv("USER")) {
    pw = getpwuid(getuid());
    if (pw)
      setenv("USER", pw->pw_name, 1);
  }

  setenv("SHELL", "zterm", 1);
}

// runs a script , zterm <file>.sh
static int run_script(const char *filename) {
  FILE *f = fopen(filename, "r"); // reads file

  if (!f) {
    fprintf(stderr, "zterm: %s: %s\n", filename, strerror(errno));
    return 1;
  }

  char buf[MAX_INPUT]; // makes a buffer
  while (fgets(buf, sizeof(buf), f)) {
    buf[strcspn(buf, "\n")] = '\0';
    // skip new lines and commentd
    if (!buf[0] || buf[0] == '#')
      continue;
    execute_line(buf);
  }

  fclose(f);            // close file
  return g_last_status; // exec code
}

static void run_interactive(void) {
  printf("zterm v1.0 type help for commands\n");

  while (1) {
    print_prompt();

    char *line = read_line();
    if (!line) {
      printf("\nexit\n");
      break;
    }

    // removes white spaces in the first of the user prompt
    while (*line == ' ' || *line == '\t')
      line++;

    if (!*line)
      continue;

    history_add(line);
    execute_line(line);
  }
}

int main(int argc, char *argv[]) {
  g_interactive = isatty(STDIN_FILENO);

  if (g_interactive)
    tcgetattr(STDIN_FILENO, &g_orig_termios);

  setup_signals();
  bootstrap_env();
  history_load();
  load_rc();

  // Non-interactive modes
  if (argc > 1) {
    g_interactive = 0;

    if (strcmp(argv[1], "-c") == 0) {
      // zterm -c "command"
      if (argc < 3) {
        fprintf(stderr, "zterm: -c requires an argument\n");
        return 1;
      }
      int r = execute_line(argv[2]); // returns status of the command
      history_save();
      return r;
    }

    // zterm script.sh
    int r = run_script(argv[1]);
    history_save();
    return r;
  }

  // REPL
  if (g_interactive)
    run_interactive();
  else {
    // stdin is a pipe/redirect in non-interactive mode
    char *line;
    while ((line = read_line()) != NULL) {
      while (*line == ' ' || *line == '\t')
        line++;
      if (!*line)
        continue;
      history_add(line);
      execute_line(line);
    }
  }

  history_save();
  if (g_interactive)
    term_restore();     // resets term state
  return g_last_status; // exit code
}
