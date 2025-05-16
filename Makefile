CC := gcc
CFLAGS := -Wall -Wextra -pthread
LDFLAGS := -pthread
SRC_DIR := source
LIBS_DIR := libs
BIN_DIR := bin

all: $(BIN_DIR) server client

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

server: $(BIN_DIR)/server

client: $(BIN_DIR)/client

$(BIN_DIR)/server: $(SRC_DIR)/server.c $(SRC_DIR)/document.c $(SRC_DIR)/markdown.c $(LIBS_DIR)/document.h $(LIBS_DIR)/markdown.h
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/server.c $(SRC_DIR)/document.c $(SRC_DIR)/markdown.c $(LDFLAGS)

$(BIN_DIR)/client: $(SRC_DIR)/client.c $(SRC_DIR)/document.c $(SRC_DIR)/markdown.c $(LIBS_DIR)/document.h $(LIBS_DIR)/markdown.h
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/client.c $(SRC_DIR)/document.c $(SRC_DIR)/markdown.c $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)
	rm -f FIFO_*
	rm -f doc.md
