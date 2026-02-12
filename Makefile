CC=gcc
CFLAGS=-O2 -Wall -Wextra
LDFLAGS=-lncurses

all: nft-tui

nft-tui: nft_tui.c
	$(CC) $(CFLAGS) -o nft-tui nft_tui.c $(LDFLAGS)

clean:
	rm -f nft-tui
