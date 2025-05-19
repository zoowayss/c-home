CC := gcc
CFLAGS := -Wall -Wextra -pthread
LDFLAGS := -pthread

all: server client

server: source/server.c source/document.c source/markdown.c
	$(CC) $(CFLAGS) -o server source/server.c source/document.c source/markdown.c $(LDFLAGS)

client: source/client.c
	$(CC) $(CFLAGS) -o client source/client.c $(LDFLAGS)

clean:
	rm -f server client *.o FIFO_* doc.md
