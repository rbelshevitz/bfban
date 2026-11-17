CC ?= cc
CFLAGS ?= -O2 -g -std=c17 -Wall -Wextra -Wpedantic

-include config.mk

PREFIX ?= /usr/local
BFBAN_DIRECT_CFLAGS ?=
BFBAN_DIRECT_LIBS ?=

.PHONY: all clean install

all: bfban bfband bfbandctl bfobserve

bfban: src/bfban.c
	$(CC) $(CFLAGS) -o $@ $<

bfband: src/bfband.c
	$(CC) $(CFLAGS) $(BFBAN_DIRECT_CFLAGS) -o $@ $< $(BFBAN_DIRECT_LIBS)

bfbandctl: src/bfbandctl.c
	$(CC) $(CFLAGS) -o $@ $<

bfobserve: src/bfobserve.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 0755 bfban $(DESTDIR)$(PREFIX)/sbin/bfban
	install -m 0755 bfband $(DESTDIR)$(PREFIX)/sbin/bfband
	install -m 0755 bfbandctl $(DESTDIR)$(PREFIX)/sbin/bfbandctl
	install -m 0755 bfobserve $(DESTDIR)$(PREFIX)/sbin/bfobserve
	install -d $(DESTDIR)/etc/fail2ban/action.d
	install -m 0644 etc/fail2ban/action.d/bfban.conf $(DESTDIR)/etc/fail2ban/action.d/bfban.conf
	install -d $(DESTDIR)/etc/bfban
	install -m 0644 etc/bfban/bfban.env $(DESTDIR)/etc/bfban/bfban.env
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 etc/systemd/bfband.service $(DESTDIR)/etc/systemd/system/bfband.service

clean:
	rm -f bfban bfband bfbandctl bfobserve
