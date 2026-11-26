SHELL := /bin/bash

RM	:=rm -f

ROOT := $(shell pwd)
OPTIMIZE ?= -O0
export OPTIMIZE
JOBS ?= $(shell nproc)
export JOBS

MAKEOVERRIDES := $(filter-out COMPILE_COMMANDS_JSON=% CXX=%,$(MAKEOVERRIDES))
unexport COMPILE_COMMANDS_JSON CXX

.PHONY:: all build
.PHONY:: clean clean-src

all:: build

include ncurses.mk
include freetype.mk
include oksh.mk
include toybox.mk
include sbase.mk
include ubase.mk

# ncurses programs

NCURSES_SOURCES	:= $(wildcard ncurses/*.c)
NCURSES_SOURCES	+= $(wildcard ncurses/*/*.c)
NCURSES_EXES	:= $(patsubst %.c,%.exe, $(NCURSES_SOURCES))

.PHONY build:: build-ncurses-programs
build-ncurses-programs: $(NCURSES_EXES)

.PHONY clean:: clean-ncurses-programs
clean-ncurses-programs:
	$(RM) $(NCURSES_EXES)

ncurses/%.exe: ncurses/%.c
	# source files must be before libs
	$(CC) -o $@ $< \
		-I$(NCURSES_INSTALL_DIR)/include -I$(NCURSES_INSTALL_DIR)/include/ncursesw \
		-ggdb $(OPTIMIZE) -fno-omit-frame-pointer \
		-L$(NCURSES_INSTALL_DIR)/lib -static \
		-lncurses

include vim.mk
include libevent.mk
include tmux.mk
include nano.mk
include less.mk

include readline.mk
include zlib.mk
include libpng.mk
include sqlite.mk
include openssl.mk
include curl.mk
include wget.mk
include nettools.mk

include python.mk

MARIO_UI_DIR := $(abspath $(ROOT)/../gui/libmario-ui)
MARIO_UI_LIB := $(MARIO_UI_DIR)/libmario-ui.a
MARIO_UI_INPUTS := $(wildcard $(MARIO_UI_DIR)/src/*.c \
	$(MARIO_UI_DIR)/include/gui/*.h)

include video/mpv.mk
include cxx/cxx.mk

include terminal.mk
include imageviewer.mk

$(TERMINAL_EXE) $(IMAGEVIEWER_EXE) $(NCURSES_EXES) $(MARIO_UI_LIB): \
	private override export COMPILE_COMMANDS_JSON := $(COMPILE_COMMANDS_JSON)

# Rebuild the archive before linking apps whenever any UI implementation changes.
$(MARIO_UI_LIB): $(MARIO_UI_INPUTS) | build-freetype build-libpng
	$(MAKE) -C $(MARIO_UI_DIR)
