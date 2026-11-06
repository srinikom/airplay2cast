# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -D_GNU_SOURCE -pthread
override LDLIBS += -lgnutls -lm -pthread

SRCS := src/main.c src/bridge.c src/cast.c src/cast_proto.c src/json.c src/mdns.c src/stream.c src/log.c
OBJS := $(SRCS:.c=.o)
LIB_SRCS := $(filter-out src/main.c,$(SRCS))
TESTS := tests/test_unit

all: airplay2cast

airplay2cast: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_unit: tests/test_unit.c $(LIB_SRCS) src/*.h
	$(CC) $(CFLAGS) -Isrc -fsanitize=address,undefined -o $@ tests/test_unit.c $(LIB_SRCS) $(LDLIBS)

test: $(TESTS)
	./tests/test_unit

install: airplay2cast
	install -D -m 755 airplay2cast $(DESTDIR)/usr/local/bin/airplay2cast

clean:
	rm -f airplay2cast $(OBJS) $(TESTS)

.PHONY: all test install clean
