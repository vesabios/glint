# Glint — cross-platform Makefile (Linux, macOS, Windows/MinGW)

CC ?= gcc
CFLAGS = -O2 -Wall -Wno-unused-function

# Detect OS
UNAME := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME),Darwin)
  # macOS — try brew, then sdl2-config
  SDL2_PREFIX := $(shell brew --prefix sdl2 2>/dev/null)
  ifneq ($(SDL2_PREFIX),)
    SDL_CFLAGS = -I$(SDL2_PREFIX)/include
    SDL_LIBS = -L$(SDL2_PREFIX)/lib -lSDL2 -lm
  else
    SDL_CFLAGS = $(shell sdl2-config --cflags)
    SDL_LIBS = $(shell sdl2-config --libs) -lm
  endif
  TARGET = glint
else ifeq ($(OS),Windows_NT)
  # Windows (MinGW/MSYS2)
  SDL_CFLAGS = $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2)
  SDL_LIBS = $(shell sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2) -lm
  TARGET = glint.exe
else
  # Linux
  SDL_CFLAGS = $(shell sdl2-config --cflags)
  SDL_LIBS = $(shell sdl2-config --libs) -lm
  TARGET = glint
endif

LDFLAGS = $(SDL_LIBS)

# wasm3 sources
W3_DIR = wasm3
W3_SRC = $(wildcard $(W3_DIR)/*.c)
W3_OBJ = $(W3_SRC:.c=.o)

all: $(TARGET)

$(TARGET): glint.o $(W3_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

glint.o: glint.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I$(W3_DIR) -c -o $@ $<

$(W3_DIR)/%.o: $(W3_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) glint.o $(W3_OBJ)

.PHONY: all clean
