# PS4 Direct Downloader - Dual Makefile (Desktop Simulation & OpenOrbis PS4 PKG)

CC ?= gcc
SRCS = src/main.c \
       src/ui.c \
       src/downloader.c \
       src/resolver.c \
       src/dlps.c \
       src/installer.c \
       src/config.c \
       src/cJSON.c

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
OO_PS4_TOOLCHAIN ?= /opt/OpenOrbis/PS4Toolchain
PS4_CC = $(OO_PS4_TOOLCHAIN)/bin/linux/clang
PS4_CREATE_EBOOT = $(OO_PS4_TOOLCHAIN)/bin/linux/create-eboot

PS4_CFLAGS = -target x86_64-scei-ps4-elf -D__ORBIS__ -D__PS4__ -O2 -Wall \
             -Isrc \
             -I$(OO_PS4_TOOLCHAIN)/include \
             -I$(OO_PS4_TOOLCHAIN)/include/SDL2

PS4_LDFLAGS = -target x86_64-scei-ps4-elf \
              -L$(OO_PS4_TOOLCHAIN)/lib \
              -lSDL2 -lcurl -lssl -lcrypto \
              -lSceVideoOut -lScePad -lSceSysmodule -lSceUserService \
              -lSceNet -lSceNetCtl -lScePosix -lc -lm

.PHONY: all desktop ps4 pkg clean setup-toolchain run-sim

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
	@echo " Esegui 'make pkg' o './build-pkg.sh' per generare il .pkg"
	@echo "=========================================================="

$(PS4_EBOOT): $(SRCS)
	@mkdir -p $(BUILD_DIR)
	$(PS4_CC) $(PS4_CFLAGS) $^ $(PS4_LDFLAGS) -o $(PS4_ELF)
	$(PS4_CREATE_EBOOT) --in=$(PS4_ELF) --out=$@

pkg:
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
