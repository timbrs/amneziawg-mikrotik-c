CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
LDFLAGS ?=

SRC = \
	src/awg_bundle.c \
	src/launcher.c \
	src/main.c \
	src/routeros_api.c \
	src/routeros_reconcile.c

OBJ = $(SRC:.c=.o)
BIN = awg-routeros-app

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c src/awg_bundle.h src/launcher.h src/routeros_api.h src/routeros_reconcile.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
