# Makefile — Conquista del territorio (client/server in C, socket TCP)
#
# Target:
#   make          -> compila server e client
#   make server   -> compila solo il server
#   make client   -> compila solo il client
#   make clean    -> rimuove eseguibili e file oggetto

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -O2

all: server client

server: server.o game.o users.o net_util.o
	$(CC) $(CFLAGS) -o $@ $^

client: client.o net_util.o
	$(CC) $(CFLAGS) -o $@ $^

# Regola generica di compilazione
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Dipendenze dagli header
net_util.o: net_util.c common.h net_util.h
users.o:    users.c    common.h users.h
game.o:     game.c     common.h protocol.h net_util.h game.h
server.o:   server.c   common.h protocol.h net_util.h game.h users.h
client.o:   client.c   common.h protocol.h net_util.h

clean:
	rm -f server client *.o

.PHONY: all clean
