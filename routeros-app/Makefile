VERSION ?= dev
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -ffunction-sections -fdata-sections -flto -DVERSION=\"$(VERSION)\"
LDFLAGS ?= -static -Wl,--gc-sections -flto -s
PROXY_LDLIBS ?= -lpthread

APP_SRC = \
	src/app_log.c \
	src/awg_bundle.c \
	src/launcher.c \
	src/main.c \
	src/routeros_api.c \
	src/routeros_reconcile.c

PROXY_SRC = \
	proxy-src/main.c \
	proxy-src/proxy.c \
	proxy-src/transform.c \
	proxy-src/blake2s.c \
	proxy-src/cps.c \
	proxy-src/fastrand.c \
	proxy-src/base64.c \
	proxy-src/log.c

APP_OBJ = $(APP_SRC:.c=.o)
PROXY_OBJ = $(PROXY_SRC:.c=.o)
APP_BIN = awg-routeros-app
PROXY_BIN = awg-proxy

.PHONY: all clean

all: $(PROXY_BIN) $(APP_BIN)

$(APP_BIN): $(APP_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APP_OBJ)

$(PROXY_BIN): $(PROXY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PROXY_OBJ) $(PROXY_LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(APP_OBJ) $(PROXY_OBJ) $(APP_BIN) $(PROXY_BIN)
