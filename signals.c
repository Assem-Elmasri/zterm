/*
 * signals.c — UNIX signal handling.
 *
 * SIGINT  — forwards to the foreground child, or reprints the prompt.
 * SIGCHLD — reaps background children without leaving zombies.
 * SIGTTOU / SIGTTIN — ignored so the shell doesn't stop on terminal I/O.
 */

#include "zterm.h"

/* ── Signal handlers ───────────────────────────────────────────────────── */

static void sigint_handler(int sig)
{
    (void)sig;
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGINT);
    } else {
        write(STDOUT_FILENO, "\n", 1);
        print_prompt();
        fflush(stdout);
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;
    /* Reap all finished background children */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        (void)status;   /* background job finished — nothing to report */
}

/* ── Public setup ──────────────────────────────────────────────────────── */

void setup_signals(void)
{
    struct sigaction sa_int  = {0};
    struct sigaction sa_chld = {0};

    sa_int.sa_handler = sigint_handler;
    sa_int.sa_flags   = SA_RESTART;
    sigaction(SIGINT, &sa_int, NULL);

    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
}
