# =========================================================
# Makefile  –  Concurrent Lost & Found System
# =========================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -Wno-unused-variable -g -std=c11 -D_POSIX_C_SOURCE=200809L
LIBS    = -pthread -lrt

TARGETS = server client matcher

.PHONY: all clean reset

all: $(TARGETS)

server: server.c common.h
	$(CC) $(CFLAGS) -o server server.c $(LIBS)
	@echo "  [OK] server compiled"

client: client.c common.h
	$(CC) $(CFLAGS) -o client client.c $(LIBS)
	@echo "  [OK] client compiled"

matcher: matcher.c common.h
	$(CC) $(CFLAGS) -o matcher matcher.c $(LIBS)
	@echo "  [OK] matcher compiled"

clean:
	rm -f $(TARGETS) items.dat users.dat
	# Remove any leftover POSIX MQs
	-rm -f /dev/mqueue/laf_new_found /dev/mqueue/laf_match_result

reset:
	rm -f items.dat users.dat
	@echo "  [OK] Data files removed. Server will re-seed on next start."
