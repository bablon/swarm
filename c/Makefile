CFLAGS += -g -Wall
CFLAGS += $(shell pkg-config --cflags gio-2.0)
LIBS += $(shell pkg-config --libs gio-2.0)

all: stun

stun: stun.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

.PHONY: clean

clean:
	$(RM) stun
