CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread
LDFLAGS=-pthread

BIN=server
SRCS=server.c http.c
OBJS=$(SRCS:.c=.o)

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c http.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) 8888

clean:
	rm -f $(BIN) $(OBJS)
