CC      = cc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS = -lX11
BIN     = vex
SRC     = wm.c

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

install: $(BIN)
	install -Dm755 $(BIN) /usr/local/bin/$(BIN)
	install -Dm644 config.conf /etc/vex/config.conf

clean:
	rm -f $(BIN)

.PHONY: install clean
