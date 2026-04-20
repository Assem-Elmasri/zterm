# ── zterm Makefile ──────────────────────────────────────────────────────────
#
# Targets:
#   make          Build the release binary
#   make debug    Build with AddressSanitizer + debug symbols
#   make clean    Remove build artefacts
#
# Usage:
#   ./zterm                   Interactive shell
#   ./zterm script.sh         Run a script file
#   ./zterm -c "echo hello"   Run a single command

CC      := gcc
TARGET  := zterm
CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 -O2
LDFLAGS :=

SRCS := main.c      \
        history.c   \
        signals.c   \
        terminal.c  \
        expand.c    \
        parser.c    \
        alias.c     \
        builtins.c  \
        exec.c

OBJS := $(SRCS:.c=.o)

# ── Default target ────────────────────────────────────────────────────────
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c zterm.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Debug build ───────────────────────────────────────────────────────────
.PHONY: debug
debug: CFLAGS += -g -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)

# ── Housekeeping ──────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET)
