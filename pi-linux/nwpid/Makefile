CC ?= gcc
CFLAGS = -Wall -Wextra -O2
TARGET = nwpid

SRCS = nwpid.c keyboard.c
OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

nwpid.o: nwpid.c protocol.h keyboard.h
keyboard.o: keyboard.c keyboard.h

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: clean
