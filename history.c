

#include "zterm.h"

History g_history = {0};

void history_add(const char *line) {
  if (!line || !*line)
    return;

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
  const char *home = getenv("HOME");
  if (!home)
    return;

  char path[512];
  snprintf(path, sizeof(path), "%s/.zterm_history", home);

  FILE *f = fopen(path, "w");
  if (!f)
    return;

  for (int i = 0; i < g_history.count; i++)
    fprintf(f, "%s\n", g_history.entries[i]);

  fclose(f);
}

void history_load(void) {
  const char *home = getenv("HOME");
  if (!home)
    return;

  char path[512];
  snprintf(path, sizeof(path), "%s/.zterm_history", home);

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
