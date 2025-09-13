CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -Isrc
LDFLAGS ?= -lm

# Optional SDL2 (headless fallback if not found)
PKG_CONFIG ?= pkg-config
SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL2_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
ifneq ($(SDL2_CFLAGS),)
  CFLAGS += $(SDL2_CFLAGS) -DHAVE_SDL2=1
  LDFLAGS += $(SDL2_LIBS)
  HAVE_SDL2 := 1
else
  $(info SDL2 not found (pkg-config sdl2). Building headless.)
endif

SRC := \
  src/main.c \
  src/util.c \
  src/nes.c \
  src/bus.c \
  src/cpu.c \
  src/ppu.c \
  src/cartridge.c \
  src/controller.c \
  src/video.c \
  src/apu.c


OBJ := $(SRC:.c=.o)

BIN := nes_emu

.PHONY: all clean debug

all: $(BIN)

debug: CFLAGS += -O0 -g -DDEBUG
debug: clean all

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)
