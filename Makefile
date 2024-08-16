# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -pedantic -std=c99 -O2

# Linker flags
LDFLAGS = -lm -pthread

# SDL2 flags (for both server and client)
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LDFLAGS = $(shell sdl2-config --libs)

# Source files
SERVER_SRC = server.c
CLIENT_SRC = client.c

# Executables
SERVER = server
CLIENT = client

# Default target
all: $(SERVER) $(CLIENT)

# Server compilation
$(SERVER): $(SERVER_SRC)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LDFLAGS)

# Client compilation
$(CLIENT): $(CLIENT_SRC)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LDFLAGS)

# Clean up
clean:
	rm -f $(SERVER) $(CLIENT)

# Phony targets
.PHONY: all clean
