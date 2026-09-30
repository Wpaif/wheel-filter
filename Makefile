PREFIX ?= /usr/local
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

# A interface gráfica só é compilada se o GTK 4 for encontrado.
GTK_CFLAGS := $(shell pkg-config --cflags gtk4 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs gtk4 2>/dev/null)

CORE = main.c filter.c io.c
ifneq ($(GTK_LIBS),)
SRC = $(CORE) devices.c gui.c
EXTRA = -DWITH_GUI $(GTK_CFLAGS)
LIBS = $(GTK_LIBS)
else
SRC = $(CORE)
EXTRA =
LIBS =
endif

wheel-filter: $(SRC) filter.h io.h devices.h gui.h
	$(CC) $(CFLAGS) $(EXTRA) -o $@ $(SRC) $(LIBS)
ifeq ($(GTK_LIBS),)
	@echo "aviso: GTK 4 não encontrado; compilado só o modo linha de comando."
endif

test_filter: test_filter.c filter.c filter.h
	$(CC) $(CFLAGS) -o $@ test_filter.c filter.c

test: test_filter
	./test_filter

install: wheel-filter
	install -Dm755 wheel-filter $(DESTDIR)$(PREFIX)/bin/wheel-filter
	install -Dm644 wheel-filter.service $(DESTDIR)/etc/systemd/system/wheel-filter.service
	echo uinput > uinput.conf.tmp
	install -Dm644 uinput.conf.tmp $(DESTDIR)/etc/modules-load.d/wheel-filter.conf
	rm -f uinput.conf.tmp
ifneq ($(GTK_LIBS),)
	install -Dm644 wheel-filter.desktop $(DESTDIR)/usr/share/applications/wheel-filter.desktop
	install -Dm644 org.wheelfilter.policy $(DESTDIR)/usr/share/polkit-1/actions/org.wheelfilter.policy
endif
ifeq ($(DESTDIR),)
	-modprobe uinput
	systemctl daemon-reload
	systemctl enable wheel-filter
	systemctl restart wheel-filter
endif

uninstall:
	-systemctl disable --now wheel-filter
	rm -f $(PREFIX)/bin/wheel-filter /etc/systemd/system/wheel-filter.service /etc/modules-load.d/wheel-filter.conf
	rm -f /usr/share/applications/wheel-filter.desktop /usr/share/polkit-1/actions/org.wheelfilter.policy
	systemctl daemon-reload

clean:
	rm -f wheel-filter test_filter

.PHONY: test install uninstall clean
