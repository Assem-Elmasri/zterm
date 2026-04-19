/*
 * parser.c — Lexer and command/pipeline parser.
 *
 * The lexer converts a raw input string into a stream of typed tokens.
 * parse_command() consumes tokens to build a single Command.
 * parse_pipeline() assembles one or more piped Commands into a Pipeline.
 *
 * Token types and Lexer state are private to this translation unit.
 */

#include "zterm.h"

/* ── Token types ───────────────────────────────────────────────────────── */

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
} Lexer;

/* ══════════════════════════════════════════════════════════════════════════
   Lexer
   ══════════════════════════════════════════════════════════════════════════ */

static Token lexer_next(Lexer *l) {
  Token tok = {0};
  const char *s = l->src;
  int i = l->pos;

  /* Skip whitespace */
  while (s[i] == ' ' || s[i] == '\t')
    i++;

  if (!s[i]) {
    tok.type = TOK_EOF;
    l->pos = i;
    return tok;
  }

  /* Shell comment — treat as end of input */
  if (s[i] == '#') {
    tok.type = TOK_EOF;
    l->pos = (int)strlen(s);
    return tok;
  }

  /* Multi-character operators (longest match first) */
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
  }
  /* Single-character operators */
  else if (s[i] == '|') {
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
    /* Word token — handle quoting and escapes */
    tok.type = TOK_WORD;
    int wi = 0;

    while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '|' && s[i] != '>' &&
           s[i] != '<' && s[i] != '&' && s[i] != ';' && s[i] != '#' &&
           wi < MAX_INPUT - 1) {
      if (s[i] == '"') {
        /* Double-quoted string: expand escapes but not single-quotes */
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
        /* Single-quoted string: no expansion at all */
        i++;
        while (s[i] && s[i] != '\'' && wi < MAX_INPUT - 1)
          tok.value[wi++] = s[i++];
        if (s[i] == '\'')
          i++;

      } else if (s[i] == '\\' && s[i + 1]) {
        /* Backslash escape outside quotes */
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

/* ══════════════════════════════════════════════════════════════════════════
   Command parser
   ══════════════════════════════════════════════════════════════════════════ */

/* Returns the delimiter token type that terminated this command. */
static int parse_command(Lexer *l, Command *cmd) {
  memset(cmd, 0, sizeof(*cmd));
  Token tok;

  while (1) {
    tok = lexer_next(l);

    /* Tokens that end this command */
    if (tok.type == TOK_EOF || tok.type == TOK_PIPE || tok.type == TOK_AND ||
        tok.type == TOK_OR || tok.type == TOK_SEMICOLON)
      break;

    if (tok.type == TOK_BACKGROUND) {
      cmd->background = 1;
      break;
    }

    /* Redirection targets */
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
      /* Regular word: expand, then apply glob */
      char *expanded = expand_word(tok.value);

      if (strpbrk(expanded, "*?[")) {
        glob_t gb;
        if (glob(expanded, GLOB_NOCHECK | GLOB_TILDE, NULL, &gb) == 0) {
          for (size_t g = 0; g < gb.gl_pathc && cmd->argc < MAX_ARGS - 1; g++)
            cmd->args[cmd->argc++] = strdup(gb.gl_pathv[g]);
          globfree(&gb);
          free(expanded);
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
  return tok.type;
}

/* ══════════════════════════════════════════════════════════════════════════
   Pipeline parser  (public API)
   ══════════════════════════════════════════════════════════════════════════ */

int parse_pipeline(const char *line, Pipeline *pl) {
  memset(pl, 0, sizeof(*pl));
  Lexer l = {.src = line, .pos = 0};

  while (pl->count < MAX_ARGS) {
    int delim = parse_command(&l, &pl->cmds[pl->count]);

    /* Skip empty commands (e.g. leading/trailing separators) */
    if (pl->cmds[pl->count].argc == 0 && !pl->cmds[pl->count].redir.input_file)
      break;

    pl->count++;

    if (delim == TOK_EOF || delim == TOK_SEMICOLON || delim == TOK_AND ||
        delim == TOK_OR || delim != TOK_PIPE)
      break;
  }

  return pl->count;
}