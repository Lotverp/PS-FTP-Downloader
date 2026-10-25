CC      = gcc
TARGET  = ps-ftp-downloader
SRC     = main.c

PKG_DEPS   = gtk4 libcurl json-glib-1.0 gdk-pixbuf-2.0
VERSION    = 1.3.5
ARCH       = amd64

CFLAGS  = $(shell pkg-config --cflags $(PKG_DEPS)) -Wall -Wextra -O2 -g
LIBS    = $(shell pkg-config --libs   $(PKG_DEPS)) -lpthread -lm

PREFIX        ?= /usr/local
BINDIR         = $(PREFIX)/bin
DATADIR        = $(PREFIX)/share/$(TARGET)
APPDIR         = $(PREFIX)/share/applications
PIXMAPSDIR     = $(PREFIX)/share/pixmaps

.PHONY: all clean install uninstall deb deb-install run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)
	@echo ""
	@echo "  [OK] Build completed -> ./$(TARGET)"
	@echo "  Run with: ./$(TARGET)"
	@echo ""

run: all
	./$(TARGET)

install: all
	install -Dm755 $(TARGET)                    $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 style.css                    $(DESTDIR)$(DATADIR)/style.css
	install -Dm755 uc_resolver.py               $(DESTDIR)$(DATADIR)/uc_resolver.py
	install -Dm755 setup-bot-resolver.sh        $(DESTDIR)$(DATADIR)/setup-bot-resolver.sh
	install -Dm644 ps-ftp-downloader.desktop    $(DESTDIR)$(APPDIR)/ps-ftp-downloader.desktop
	if [ -f "ps-ftp-downloader.png" ]; then \
	    install -Dm644 ps-ftp-downloader.png    $(DESTDIR)$(PIXMAPSDIR)/ps-ftp-downloader.png; \
	fi
	@command -v update-desktop-database >/dev/null 2>&1 && \
	    update-desktop-database $(DESTDIR)$(APPDIR) || true
	@echo "  [OK] Installed to $(BINDIR)/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -rf $(DESTDIR)$(DATADIR)
	rm -f $(DESTDIR)$(APPDIR)/ps-ftp-downloader.desktop
	rm -f $(DESTDIR)$(PIXMAPSDIR)/ps-ftp-downloader.png

deb: all
	chmod +x build-deb.sh
	./build-deb.sh

deb-install: deb
	pkexec apt-get install -y --reinstall $$(realpath ../$(TARGET)_$(VERSION)_$(ARCH).deb)

clean:
	rm -f $(TARGET)
	@echo "  [OK] Cleaned"
