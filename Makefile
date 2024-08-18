# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -pedantic -std=c99 -O2

# Linker flags
LDFLAGS = -lm -pthread

# SDL2 flags (for both server and client)
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LDFLAGS = $(shell sdl2-config --libs)

# SDL2_ttf flags
TTF_CFLAGS = $(shell pkg-config --cflags SDL2_ttf)
TTF_LDFLAGS = $(shell pkg-config --libs SDL2_ttf)

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
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TTF_CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LDFLAGS) $(TTF_LDFLAGS)

# Clean up
clean:
	rm -f $(SERVER) $(CLIENT)

# Phony targets
.PHONY: all clean
