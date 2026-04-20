/*
 * main.c — Shell entry point.
 *
 * Handles:
 *   - Interactive REPL (default when stdin is a terminal)
 *   - Script file execution  (msh script.sh)
 *   - Inline command via -c  (msh -c "echo hello")
 */

#include "zterm.h"

/* ══════════════════════════════════════════════════════════════════════════
   Environment bootstrap
   ══════════════════════════════════════════════════════════════════════════ */

static void bootstrap_env(void)
{
    struct passwd *pw;

    if (!getenv("HOME")) {
        pw = getpwuid(getuid());
        if (pw) setenv("HOME", pw->pw_dir, 1);
    }

    if (!getenv("PATH"))
        setenv("PATH",
               "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin",
               1);

    if (!getenv("USER")) {
        pw = getpwuid(getuid());
        if (pw) setenv("USER", pw->pw_name, 1);
    }

    setenv("SHELL", "msh", 1);
}

/* ══════════════════════════════════════════════════════════════════════════
   Script-file mode
   ══════════════════════════════════════════════════════════════════════════ */

static int run_script(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "msh: %s: %s\n", filename, strerror(errno));
        return 1;
    }

    char buf[MAX_INPUT];
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        /* Skip empty lines, comments, and the shebang */
        if (!buf[0] || buf[0] == '#') continue;
        execute_line(buf);
    }

    fclose(f);
    return g_last_status;
}

/* ══════════════════════════════════════════════════════════════════════════
   Interactive REPL
   ══════════════════════════════════════════════════════════════════════════ */

static void run_interactive(void)
{
    printf("%smsh%s v1.0  —  type %shelp%s for commands\n",
           COLOR_BOLD COLOR_CYAN, COLOR_RESET, COLOR_BOLD, COLOR_RESET);

    while (1) {
        print_prompt();

        char *line = read_line();
        if (!line) {
            printf("\nexit\n");
            break;
        }

        /* Trim leading whitespace */
        while (*line == ' ' || *line == '\t')
            line++;

        if (!*line) continue;

        history_add(line);
        execute_line(line);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   main
   ══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    g_interactive = isatty(STDIN_FILENO);

    if (g_interactive)
        tcgetattr(STDIN_FILENO, &g_orig_termios);

    setup_signals();
    bootstrap_env();
    history_load();
    load_rc();

    /* ── Non-interactive modes ─────────────────────────────────────────── */
    if (argc > 1) {
        g_interactive = 0;

        if (strcmp(argv[1], "-c") == 0) {
            /* msh -c "command string" */
            if (argc < 3) {
                fprintf(stderr, "msh: -c requires an argument\n");
                return 1;
            }
            int r = execute_line(argv[2]);
            history_save();
            return r;
        }

        /* msh script.sh */
        int r = run_script(argv[1]);
        history_save();
        return r;
    }

    /* ── Interactive REPL ──────────────────────────────────────────────── */
    if (g_interactive)
        run_interactive();
    else {
        /* stdin is a pipe/redirect in non-interactive mode */
        char *line;
        while ((line = read_line()) != NULL) {
            while (*line == ' ' || *line == '\t') line++;
            if (!*line) continue;
            history_add(line);
            execute_line(line);
        }
    }

    history_save();
    if (g_interactive) term_restore();
    return g_last_status;
}
