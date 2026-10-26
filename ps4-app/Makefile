# PS4 Direct Downloader - Dual Makefile (Desktop Simulation & OpenOrbis PS4 PKG)

CC ?= gcc
SRCS = src/main.c \
       src/ui.c \
       src/downloader.c \
       src/resolver.c \
       src/dlps.c \
       src/installer.c \
       src/config.c \
       src/net_client.c \
       src/cJSON.c \
       src/debug_log.c

OBJS = $(SRCS:.c=.o)
BUILD_DIR = build
SIM_BIN = ps4-downloader-sim
PS4_ELF = $(BUILD_DIR)/app.elf
PS4_EBOOT = $(BUILD_DIR)/eboot.bin

# Desktop Simulation Flags
DEPS_DIR = $(CURDIR)/deps
LOCAL_SDL_INC = -I$(DEPS_DIR)/sdl2/usr/include -I$(DEPS_DIR)/sdl2/usr/include/x86_64-linux-gnu
LOCAL_SDL_LIB = -L$(DEPS_DIR)/sdl2/usr/lib/x86_64-linux-gnu -lSDL2

SYS_SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SYS_SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

ifeq ($(strip $(SYS_SDL_CFLAGS)),)
    DESKTOP_SDL_CFLAGS = $(LOCAL_SDL_INC)
    DESKTOP_SDL_LIBS   = $(LOCAL_SDL_LIB)
else
    DESKTOP_SDL_CFLAGS = $(SYS_SDL_CFLAGS)
    DESKTOP_SDL_LIBS   = $(SYS_SDL_LIBS)
endif

CURL_CFLAGS := $(shell curl-config --cflags 2>/dev/null || echo "")
CURL_LIBS   := $(shell curl-config --libs 2>/dev/null || echo "-lcurl")

DESKTOP_CFLAGS  = -O2 -Wall -Wextra -pthread -Isrc $(DESKTOP_SDL_CFLAGS) $(CURL_CFLAGS)
DESKTOP_LDFLAGS = -pthread -lm $(DESKTOP_SDL_LIBS) $(CURL_LIBS)

# PS4 OpenOrbis Toolchain Settings
OO_PS4_TOOLCHAIN ?= $(shell if [ -d "$(HOME)/.openorbis/OpenOrbis/PS4Toolchain" ]; then echo "$(HOME)/.openorbis/OpenOrbis/PS4Toolchain"; else echo "/opt/OpenOrbis/PS4Toolchain"; fi)

PS4_CC = $(shell if [ -x "$(HOME)/.openorbis/bin/clang" ]; then echo "$(HOME)/.openorbis/bin/clang"; else echo "clang"; fi)
PS4_LD = $(shell if [ -x "$(HOME)/.openorbis/bin/ld.lld" ]; then echo "$(HOME)/.openorbis/bin/ld.lld"; else echo "ld.lld"; fi)
PS4_CREATE_FSELF = $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself
PS4_PKGTOOL = $(OO_PS4_TOOLCHAIN)/bin/linux/PkgTool.Core

PS4_CFLAGS = --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -D__ORBIS__ -D__PS4__ -O2 -Wall \
             -Isrc \
             -isysroot $(OO_PS4_TOOLCHAIN) \
             -isystem $(OO_PS4_TOOLCHAIN)/include \
             -isystem $(OO_PS4_TOOLCHAIN)/include/SDL2

PS4_LIBS = -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad \
           -lSceSysmodule -lSDL2 -lSceNet -lSceSsl -lSceHttp -lSceSystemService

PS4_LDFLAGS = -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x --eh-frame-hdr \
              -L$(OO_PS4_TOOLCHAIN)/lib $(PS4_LIBS) $(OO_PS4_TOOLCHAIN)/lib/crt1.o

PS4_OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all desktop ps4 pkg clean setup-toolchain run-sim check-toolchain

all: desktop

desktop: $(SIM_BIN)
	@echo "=========================================================="
	@echo " Desktop Simulator compilato con successo!"
	@echo " Avvia con: ./$(SIM_BIN)"
	@echo "=========================================================="

$(SIM_BIN): $(SRCS)
	$(CC) $(DESKTOP_CFLAGS) $^ $(DESKTOP_LDFLAGS) -o $@

ps4: check-toolchain $(PS4_EBOOT)
	@echo "=========================================================="
	@echo " EBOOT.BIN per PS4 compilato con successo in $(PS4_EBOOT)!"
	@echo " Esegui 'make pkg' per generare il file .pkg"
	@echo "=========================================================="

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(PS4_CC) $(PS4_CFLAGS) -c $< -o $@

$(PS4_EBOOT): $(PS4_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(PS4_LD) $(PS4_OBJS) -o $(PS4_ELF) $(PS4_LDFLAGS)
	export OO_PS4_TOOLCHAIN="$(OO_PS4_TOOLCHAIN)" && \
	$(PS4_CREATE_FSELF) -in=$(PS4_ELF) -out=$(BUILD_DIR)/app.oelf --eboot "$@" --paid 0x3800000000000011

pkg: ps4
	./build-pkg.sh

check-toolchain:
	@if [ ! -d "$(OO_PS4_TOOLCHAIN)" ]; then \
		echo "ERRORE: OpenOrbis PS4 Toolchain non trovato in $(OO_PS4_TOOLCHAIN)"; \
		echo "Esegui prima: ./setup-toolchain.sh oppure imposta OO_PS4_TOOLCHAIN"; \
		exit 1; \
	fi

setup-toolchain:
	./setup-toolchain.sh

run-sim: $(SIM_BIN)
	./$(SIM_BIN)

clean:
	rm -rf $(BUILD_DIR) $(SIM_BIN) *.pkg app.gp4
