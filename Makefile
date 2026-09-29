PREFIX ?= /usr/local
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

wheel-filter: main.c
	$(CC) $(CFLAGS) -o $@ main.c

install: wheel-filter
	install -Dm755 wheel-filter $(DESTDIR)$(PREFIX)/bin/wheel-filter
	install -Dm644 wheel-filter.service $(DESTDIR)/etc/systemd/system/wheel-filter.service
	echo uinput > uinput.conf.tmp
	install -Dm644 uinput.conf.tmp $(DESTDIR)/etc/modules-load.d/wheel-filter.conf
	rm -f uinput.conf.tmp
ifeq ($(DESTDIR),)
	-modprobe uinput
	systemctl daemon-reload
	systemctl enable wheel-filter
	systemctl restart wheel-filter
endif

uninstall:
	-systemctl disable --now wheel-filter
	rm -f $(PREFIX)/bin/wheel-filter /etc/systemd/system/wheel-filter.service /etc/modules-load.d/wheel-filter.conf
	systemctl daemon-reload

clean:
	rm -f wheel-filter

.PHONY: install uninstall clean
