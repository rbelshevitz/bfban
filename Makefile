CC ?= cc
CFLAGS ?= -O2 -g -std=c17 -Wall -Wextra -Wpedantic

-include config.mk

PREFIX ?= /usr
BUILD_DIR ?= build
BFBAN_DIRECT_CFLAGS ?=
BFBAN_DIRECT_LIBS ?=

.PHONY: all clean install test

all: $(BUILD_DIR)/bfban $(BUILD_DIR)/bfband $(BUILD_DIR)/bfbandctl $(BUILD_DIR)/bfobserve

$(BUILD_DIR):
	install -d $@

$(BUILD_DIR)/bfban: src/bfban.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/bfband: src/bfband.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(BFBAN_DIRECT_CFLAGS) -o $@ $< $(BFBAN_DIRECT_LIBS)

$(BUILD_DIR)/bfbandctl: src/bfbandctl.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/bfobserve: src/bfobserve.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/etc/bfban/bfban.env: etc/bfban/bfban.env.in | $(BUILD_DIR)
	install -d $(dir $@)
	sed -e 's|@PREFIX@|$(PREFIX)|g' $< > $@

$(BUILD_DIR)/etc/systemd/bfband.service: etc/systemd/bfband.service.in | $(BUILD_DIR)
	install -d $(dir $@)
	sed -e 's|@PREFIX@|$(PREFIX)|g' $< > $@

$(BUILD_DIR)/etc/fail2ban/action.d/bfban.conf: etc/fail2ban/action.d/bfban.conf.in | $(BUILD_DIR)
	install -d $(dir $@)
	sed -e 's|@PREFIX@|$(PREFIX)|g' $< > $@

install: all $(BUILD_DIR)/etc/bfban/bfban.env $(BUILD_DIR)/etc/systemd/bfband.service \
	$(BUILD_DIR)/etc/fail2ban/action.d/bfban.conf
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 0755 $(BUILD_DIR)/bfban $(DESTDIR)$(PREFIX)/sbin/bfban
	install -m 0755 $(BUILD_DIR)/bfband $(DESTDIR)$(PREFIX)/sbin/bfband
	install -m 0755 $(BUILD_DIR)/bfbandctl $(DESTDIR)$(PREFIX)/sbin/bfbandctl
	install -m 0755 $(BUILD_DIR)/bfobserve $(DESTDIR)$(PREFIX)/sbin/bfobserve
	install -d $(DESTDIR)/etc/fail2ban/action.d
	install -m 0644 $(BUILD_DIR)/etc/fail2ban/action.d/bfban.conf $(DESTDIR)/etc/fail2ban/action.d/bfban.conf
	install -d $(DESTDIR)/etc/bfban
	install -m 0644 $(BUILD_DIR)/etc/bfban/bfban.env $(DESTDIR)/etc/bfban/bfban.env
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 $(BUILD_DIR)/etc/systemd/bfband.service $(DESTDIR)/etc/systemd/system/bfband.service

clean:
	rm -rf $(BUILD_DIR)

TEST_BUILD_DIR ?= $(BUILD_DIR)-test
TEST_CFLAGS ?= -DBFBAN_TEST_HOOKS

test:
	$(MAKE) BUILD_DIR=$(TEST_BUILD_DIR) CFLAGS="$(CFLAGS) $(TEST_CFLAGS)" all
	sh scripts/test-faults.sh $(TEST_BUILD_DIR)
