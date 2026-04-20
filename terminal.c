/*
 * terminal.c — Terminal control, prompt rendering, and the line editor.
 *
 * The line editor supports:
 *   ↑ / ↓        history navigation
 *   ← / →        cursor movement
 *   Home / End   jump to line boundaries
 *   Delete        forward-delete character
 *   Backspace     backward-delete character
 *   Ctrl-A / E   beginning / end of line
 *   Ctrl-K        kill to end of line
 *   Ctrl-U        kill to beginning of line
 *   Ctrl-C        cancel current line
 *   Ctrl-D        EOF / exit (on empty line)
 */

#include "zterm.h"

/* ── Globals defined here ──────────────────────────────────────────────── */
struct termios g_orig_termios;
int            g_interactive = 0;

/* ── Module-private line-buffer state ─────────────────────────────────── */
static char g_linebuf[MAX_INPUT];
static int  g_linelen = 0;
static int  g_cursor  = 0;

/* ══════════════════════════════════════════════════════════════════════════
   Terminal raw / restore
   ══════════════════════════════════════════════════════════════════════════ */

void term_raw(void)
{
    struct termios raw = g_orig_termios;
    raw.c_lflag      &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]    = 1;
    raw.c_cc[VTIME]   = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

/* ── Redraw the current line buffer in-place ───────────────────────────── */
static void line_refresh(void)
{
    write(STDOUT_FILENO, "\r\033[K", 4);   /* CR + erase to end of line */
    write(STDOUT_FILENO, g_linebuf, g_linelen);

    /* Reposition cursor if it is not at the end */
    if (g_cursor < g_linelen) {
        char seq[16];
        snprintf(seq, sizeof(seq), "\033[%dD", g_linelen - g_cursor);
        write(STDOUT_FILENO, seq, strlen(seq));
    }
}

/* ── Overwrite the line area with a history entry ──────────────────────── */
static void line_load_history(int hist_pos)
{
    strncpy(g_linebuf, g_history.entries[hist_pos], MAX_INPUT - 1);
    g_linelen          = (int)strlen(g_linebuf);
    g_cursor           = g_linelen;
    write(STDOUT_FILENO, "\r\033[K", 4);
    print_prompt();
    write(STDOUT_FILENO, g_linebuf, g_linelen);
}

/* ══════════════════════════════════════════════════════════════════════════
   Prompt
   ══════════════════════════════════════════════════════════════════════════ */

void print_prompt(void)
{
    char        cwd[512];
    char        host[64];
    const char *user = getenv("USER");
    if (!user) user  = "user";

    gethostname(host, sizeof(host));
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");

    /* Shorten home directory to ~ */
    const char *home = getenv("HOME");
    char        display_cwd[512];
    if (home && strncmp(cwd, home, strlen(home)) == 0)
        snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
    else
        strncpy(display_cwd, cwd, sizeof(display_cwd));

    /* Root gets red @, others get green */
    const char *uc     = (getuid() == 0) ? COLOR_RED   : COLOR_GREEN;
    const char *symbol = (getuid() == 0) ? "#" : "$";

    printf("%s%s%s@%s%s%s:%s%s%s%s%s ",
           COLOR_BOLD, uc, user,
           host, COLOR_RESET,
           COLOR_BOLD, COLOR_BLUE, display_cwd,
           COLOR_RESET, COLOR_BOLD, symbol);
    fflush(stdout);
}

/* ══════════════════════════════════════════════════════════════════════════
   Line reader
   ══════════════════════════════════════════════════════════════════════════ */

char *read_line(void)
{
    /* Non-interactive (script / pipe): plain fgets */
    if (!g_interactive) {
        static char buf[MAX_INPUT];
        if (!fgets(buf, sizeof(buf), stdin))
            return NULL;
        buf[strcspn(buf, "\n")] = '\0';
        return buf;
    }

    /* Interactive: full line-editor */
    memset(g_linebuf, 0, sizeof(g_linebuf));
    g_linelen = 0;
    g_cursor  = 0;
    int hist_pos = g_history.count;

    term_raw();

    while (1) {
        unsigned char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            term_restore();
            return NULL;
        }

        /* ── Enter ──────────────────────────────────────────────────────── */
        if (c == '\n' || c == '\r') {
            write(STDOUT_FILENO, "\n", 1);
            break;

        /* ── Ctrl-D (EOF) ──────────────────────────────────────────────── */
        } else if (c == 4) {
            if (g_linelen == 0) {
                term_restore();
                return NULL;
            }

        /* ── Ctrl-C (cancel line) ──────────────────────────────────────── */
        } else if (c == 3) {
            write(STDOUT_FILENO, "^C\n", 3);
            term_restore();
            print_prompt();
            term_raw();
            g_linelen = 0;
            g_cursor  = 0;
            memset(g_linebuf, 0, sizeof(g_linebuf));
            hist_pos  = g_history.count;
            continue;

        /* ── Backspace ─────────────────────────────────────────────────── */
        } else if (c == 127 || c == 8) {
            if (g_cursor > 0) {
                memmove(&g_linebuf[g_cursor - 1], &g_linebuf[g_cursor],
                        g_linelen - g_cursor);
                g_cursor--;
                g_linelen--;
                g_linebuf[g_linelen] = '\0';
                line_refresh();
            }

        /* ── Escape sequences (arrow keys, Home, End, Delete) ─────────── */
        } else if (c == '\033') {
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;

            if (seq[0] == '[') {
                switch (seq[1]) {
                case 'A':   /* Up arrow — older history */
                    if (hist_pos > 0)
                        line_load_history(--hist_pos);
                    break;

                case 'B':   /* Down arrow — newer history */
                    if (hist_pos < g_history.count) {
                        hist_pos++;
                        if (hist_pos == g_history.count) {
                            g_linebuf[0] = '\0';
                            g_linelen    = 0;
                            g_cursor     = 0;
                            write(STDOUT_FILENO, "\r\033[K", 4);
                            print_prompt();
                        } else {
                            line_load_history(hist_pos);
                        }
                    }
                    break;

                case 'C':   /* Right arrow */
                    if (g_cursor < g_linelen) {
                        g_cursor++;
                        write(STDOUT_FILENO, "\033[C", 3);
                    }
                    break;

                case 'D':   /* Left arrow */
                    if (g_cursor > 0) {
                        g_cursor--;
                        write(STDOUT_FILENO, "\033[D", 3);
                    }
                    break;

                case 'H':   /* Home */
                    g_cursor = 0;
                    line_refresh();
                    break;

                case 'F':   /* End */
                    g_cursor = g_linelen;
                    line_refresh();
                    break;

                case '3': { /* Delete key: ESC[3~ */
                    unsigned char tilde;
                    read(STDIN_FILENO, &tilde, 1);
                    if (tilde == '~' && g_cursor < g_linelen) {
                        memmove(&g_linebuf[g_cursor],
                                &g_linebuf[g_cursor + 1],
                                g_linelen - g_cursor - 1);
                        g_linelen--;
                        g_linebuf[g_linelen] = '\0';
                        line_refresh();
                    }
                    break;
                }
                }
            }

        /* ── Ctrl-A: beginning of line ─────────────────────────────────── */
        } else if (c == 1) {
            g_cursor = 0;
            line_refresh();

        /* ── Ctrl-E: end of line ───────────────────────────────────────── */
        } else if (c == 5) {
            g_cursor = g_linelen;
            line_refresh();

        /* ── Ctrl-K: kill to end of line ───────────────────────────────── */
        } else if (c == 11) {
            g_linelen            = g_cursor;
            g_linebuf[g_linelen] = '\0';
            line_refresh();

        /* ── Ctrl-U: kill to beginning of line ─────────────────────────── */
        } else if (c == 21) {
            memmove(g_linebuf, &g_linebuf[g_cursor], g_linelen - g_cursor);
            g_linelen           -= g_cursor;
            g_cursor             = 0;
            g_linebuf[g_linelen] = '\0';
            line_refresh();

        /* ── Printable character ───────────────────────────────────────── */
        } else if (isprint(c)) {
            if (g_linelen < MAX_INPUT - 1) {
                memmove(&g_linebuf[g_cursor + 1], &g_linebuf[g_cursor],
                        g_linelen - g_cursor);
                g_linebuf[g_cursor++] = c;
                g_linelen++;
                g_linebuf[g_linelen]  = '\0';
                line_refresh();
            }
        }
    }

    term_restore();
    return g_linebuf;
}
