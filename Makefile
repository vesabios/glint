CC = gcc
CFLAGS = -O2 -Wall -Wno-unused-function
LDFLAGS = $(shell sdl2-config --libs) -lm
SDL_CFLAGS = $(shell sdl2-config --cflags)

# wasm3 sources
W3_DIR = wasm3
W3_SRC = $(wildcard $(W3_DIR)/*.c)
W3_OBJ = $(W3_SRC:.c=.o)

TARGET = glint

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
