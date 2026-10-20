CC      = clang
CFLAGS  = -Wall -Wextra -O2
LDLIBS  = -framework IOKit -framework CoreFoundation
BIN     = fancurve
SRC     = main.c smc.c
PREFIX  = /usr/local
PLIST   = com.fancurve.curve.plist
DAEMON  = /Library/LaunchDaemons/$(PLIST)

all: $(BIN) fancurved

$(BIN): $(SRC) smc.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDLIBS)

# Root daemon used by the GUI app. powerstate.c is daemon-only — the app has no
# use for sleep/lid detection, so it stays out of build_app.sh.
fancurved: fancurved.c fanapi.c smc.c powerstate.c fanapi.h smc.h powerstate.h
	$(CC) $(CFLAGS) -o fancurved fancurved.c fanapi.c smc.c powerstate.c $(LDLIBS)

# Install the binary to $(PREFIX)/bin (needs sudo).
install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	@echo "installed to $(PREFIX)/bin/$(BIN)"

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)

# Install + load the launchd daemon so the curve runs at boot (needs sudo).
# Reads LOW/HIGH/SEC from the plist. Run `make install` first.
daemon: install
	install -m 644 $(PLIST) $(DAEMON)
	launchctl bootout system $(DAEMON) 2>/dev/null || true
	launchctl bootstrap system $(DAEMON)
	@echo "daemon loaded: $(DAEMON)"

undaemon:
	launchctl bootout system $(DAEMON) 2>/dev/null || true
	rm -f $(DAEMON)
	@echo "daemon removed"

clean:
	rm -f $(BIN) fancurved fanapi.o smc.o powerstate.o main.o

.PHONY: all install uninstall daemon undaemon clean
