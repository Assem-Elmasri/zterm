/*
 * expand.c — Word expansion: variables, tilde, globs, command substitution.
 *
 * expand_word() is the single entry-point used by the parser.  It chains
 * tilde → variable → command-substitution expansion and returns a
 * heap-allocated string that the caller must free.
 */

#include "zterm.h"

/* ══════════════════════════════════════════════════════════════════════════
   Variable expansion  ($VAR, ${VAR}, $?)
   ══════════════════════════════════════════════════════════════════════════ */

char *expand_vars(const char *s)
{
    char out[MAX_INPUT * 4];
    int  oi  = 0;
    int  si  = 0;
    int  len = (int)strlen(s);

    while (si < len && oi < (int)sizeof(out) - 1) {

        /* Backslash escape */
        if (s[si] == '\\' && si + 1 < len) {
            out[oi++] = s[++si];
            si++;
            continue;
        }

        if (s[si] != '$') {
            out[oi++] = s[si++];
            continue;
        }

        /* '$' found — decide which expansion */
        si++;

        if (s[si] == '?') {
            /* Exit status */
            char num[16];
            int  nl = snprintf(num, sizeof(num), "%d", g_last_status);
            if (oi + nl < (int)sizeof(out) - 1) {
                memcpy(&out[oi], num, nl);
                oi += nl;
            }
            si++;

        } else if (s[si] == '{') {
            /* ${VAR} */
            si++;
            char var[256];
            int  vi = 0;
            while (s[si] && s[si] != '}' && vi < 255)
                var[vi++] = s[si++];
            var[vi] = '\0';
            if (s[si] == '}') si++;

            const char *v = getenv(var);
            if (v) {
                int vl = (int)strlen(v);
                if (oi + vl < (int)sizeof(out) - 1) {
                    memcpy(&out[oi], v, vl);
                    oi += vl;
                }
            }

        } else if (isalpha((unsigned char)s[si]) || s[si] == '_') {
            /* $VAR */
            char var[256];
            int  vi = 0;
            while ((isalnum((unsigned char)s[si]) || s[si] == '_') && vi < 255)
                var[vi++] = s[si++];
            var[vi] = '\0';

            const char *v = getenv(var);
            if (v) {
                int vl = (int)strlen(v);
                if (oi + vl < (int)sizeof(out) - 1) {
                    memcpy(&out[oi], v, vl);
                    oi += vl;
                }
            }

        } else {
            /* Bare '$' — pass through */
            out[oi++] = '$';
        }
    }

    out[oi] = '\0';
    return strdup(out);
}

/* ══════════════════════════════════════════════════════════════════════════
   Tilde expansion  (~, ~/path, ~username)
   ══════════════════════════════════════════════════════════════════════════ */

char *expand_tilde(const char *s)
{
    if (s[0] != '~')
        return strdup(s);

    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/";
    }

    char out[MAX_INPUT];

    if (s[1] == '/' || s[1] == '\0') {
        /* ~/path or bare ~ */
        snprintf(out, sizeof(out), "%s%s", home, s + 1);

    } else {
        /* ~username/path */
        char        uname[64];
        int         i = 0;
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

/* ══════════════════════════════════════════════════════════════════════════
   Command substitution  $(cmd)
   ══════════════════════════════════════════════════════════════════════════ */

char *expand_cmdsub(const char *s)
{
    char out[MAX_INPUT * 4];
    int  oi  = 0;
    int  si  = 0;
    int  len = (int)strlen(s);

    while (si < len && oi < (int)sizeof(out) - 2) {

        if (s[si] != '$' || s[si + 1] != '(') {
            out[oi++] = s[si++];
            continue;
        }

        /* Found $( — find the matching closing ) */
        int depth = 1;
        int start = si + 2;
        si += 2;
        while (s[si] && depth > 0) {
            if      (s[si] == '(') depth++;
            else if (s[si] == ')') depth--;
            si++;
        }

        /* Extract inner command text */
        int  ilen = si - start - 1;
        if (ilen < 0)            ilen = 0;
        if (ilen >= MAX_INPUT)   ilen = MAX_INPUT - 1;

        char inner[MAX_INPUT];
        strncpy(inner, s + start, ilen);
        inner[ilen] = '\0';

        /* Run inner command; capture its stdout */
        int pfd[2];
        if (pipe(pfd) != 0)
            continue;

        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[PIPE_READ]);
            dup2(pfd[PIPE_WRITE], STDOUT_FILENO);
            close(pfd[PIPE_WRITE]);

            Pipeline ipl;
            parse_pipeline(inner, &ipl);
            shell_execute(&ipl);
            exit(g_last_status);
        }

        close(pfd[PIPE_WRITE]);

        char buf[MAX_INPUT];
        int  nr;
        while ((nr = read(pfd[PIPE_READ], buf, sizeof(buf) - 1)) > 0) {
            buf[nr] = '\0';
            /* Strip trailing newlines */
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

    out[oi] = '\0';
    return strdup(out);
}

/* ══════════════════════════════════════════════════════════════════════════
   Full word expansion  (tilde → vars → command substitution)
   ══════════════════════════════════════════════════════════════════════════ */

char *expand_word(const char *w)
{
    char *t = expand_tilde(w);
    char *v = expand_vars(t);   free(t);
    char *c = expand_cmdsub(v); free(v);
    return c;
}
