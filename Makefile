PREFIX ?= /usr/local
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

# A interface gráfica só é compilada se o GTK 4 for encontrado.
GTK_CFLAGS := $(shell pkg-config --cflags gtk4 libadwaita-1 dbusmenu-glib-0.4 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs gtk4 libadwaita-1 dbusmenu-glib-0.4 2>/dev/null)

CORE = main.c filter.c io.c
ifneq ($(GTK_LIBS),)
SRC = $(CORE) devices.c gui.c tray.c
EXTRA = -DWITH_GUI $(GTK_CFLAGS)
LIBS = $(GTK_LIBS)
else
SRC = $(CORE)
EXTRA =
LIBS =
endif

wheel-filter: $(SRC) filter.h io.h devices.h gui.h tray.h
	$(CC) $(CFLAGS) $(EXTRA) -o $@ $(SRC) $(LIBS)
ifeq ($(GTK_LIBS),)
	@echo "aviso: GTK 4 não encontrado; compilado só o modo linha de comando."
endif

test_filter: test_filter.c filter.c filter.h
	$(CC) $(CFLAGS) -o $@ test_filter.c filter.c

test: test_filter
	./test_filter

# Os arquivos de dados trazem @PREFIX@ no lugar do caminho do binário.
define install_tpl
	sed 's|@PREFIX@|$(PREFIX)|g' $(1) > $(1).tmp
	install -Dm$(3) $(1).tmp $(DESTDIR)$(2)
	rm -f $(1).tmp
endef

ICON_DIR = /usr/share/icons/hicolor/scalable/apps

install: wheel-filter
	install -Dm755 wheel-filter $(DESTDIR)$(PREFIX)/bin/wheel-filter
	$(call install_tpl,wheel-filter.service,/etc/systemd/system/wheel-filter.service,644)
	printf 'uinput\n' > uinput.conf.tmp
	install -Dm644 uinput.conf.tmp $(DESTDIR)/etc/modules-load.d/wheel-filter.conf
	rm -f uinput.conf.tmp
ifneq ($(GTK_LIBS),)
	$(call install_tpl,org.wheelfilter.App.desktop,/usr/share/applications/org.wheelfilter.App.desktop,644)
	$(call install_tpl,org.wheelfilter.policy,/usr/share/polkit-1/actions/org.wheelfilter.policy,644)
	install -Dm644 wheel-filter.svg $(DESTDIR)$(ICON_DIR)/wheel-filter.svg
endif
ifeq ($(DESTDIR),)
	-modprobe uinput
	-gtk-update-icon-cache -q -t /usr/share/icons/hicolor
	-update-desktop-database -q /usr/share/applications
	systemctl daemon-reload
	systemctl enable wheel-filter
	systemctl restart wheel-filter
endif

# Para o serviço e qualquer motor iniciado pela interface (que também
# captura o mouse) antes de remover os arquivos.
uninstall:
ifeq ($(DESTDIR),)
	-systemctl disable --now wheel-filter
	-pkill -TERM -f '$(PREFIX)/bin/wheel-filter --engine'
endif
	rm -f $(DESTDIR)$(PREFIX)/bin/wheel-filter \
	      $(DESTDIR)/etc/systemd/system/wheel-filter.service \
	      $(DESTDIR)/etc/modules-load.d/wheel-filter.conf \
	      $(DESTDIR)/usr/share/applications/org.wheelfilter.App.desktop \
	      $(DESTDIR)/usr/share/applications/wheel-filter.desktop \
	      $(DESTDIR)/usr/share/polkit-1/actions/org.wheelfilter.policy \
	      $(DESTDIR)$(ICON_DIR)/wheel-filter.svg
ifeq ($(DESTDIR),)
	-gtk-update-icon-cache -q -t /usr/share/icons/hicolor
	-update-desktop-database -q /usr/share/applications
	systemctl daemon-reload
endif

clean:
	rm -f wheel-filter test_filter *.tmp

.PHONY: test install uninstall clean
