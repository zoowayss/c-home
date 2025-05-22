CC := gcc
CFLAGS := -Wall -Wextra -pthread
LDFLAGS := -pthread

all: server client

server: source/server.c source/document.c source/markdown.c
	$(CC) $(CFLAGS) -o server source/server.c source/document.c source/markdown.c $(LDFLAGS)

client: source/client.c source/document.c source/markdown.c
	$(CC) $(CFLAGS) -o client source/client.c source/document.c source/markdown.c $(LDFLAGS)

# Object file compilation rules
markdown.o: source/markdown.c libs/markdown.h libs/document.h
	$(CC) $(CFLAGS) -c source/markdown.c -o markdown.o

document.o: source/document.c libs/document.h
	$(CC) $(CFLAGS) -c source/document.c -o document.o

server.o: source/server.c libs/document.h libs/markdown.h
	$(CC) $(CFLAGS) -c source/server.c -o server.o

client.o: source/client.c libs/document.h libs/markdown.h
	$(CC) $(CFLAGS) -c source/client.c -o client.o

clean:
	rm -f server client *.o FIFO_* doc.md
