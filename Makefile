CC      = cc
CFLAGS  = -O2 -std=c99 -Wall -Wextra -pedantic
LDFLAGS = -lX11 -lXinerama
BIN     = vex
SRC     = wm.c

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

install: $(BIN)
	install -Dm755 $(BIN)      /usr/local/bin/$(BIN)
	install -Dm644 config.conf /etc/vex/config.conf

uninstall:
	rm -f /usr/local/bin/$(BIN)
	rm -f /etc/vex/config.conf

clean:
	rm -f $(BIN)

.PHONY: install uninstall clean
