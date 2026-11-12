SHELL := /bin/bash

RM	:=rm -f

ROOT := $(shell pwd)
OPTIMIZE ?= -O0
export OPTIMIZE
JOBS ?= $(shell nproc)
export JOBS

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

include python.mk

TERMINAL_EXE := terminal/terminal.exe
TERMINAL_UI_DIR := $(ROOT)/../gui/libmario-ui
TERMINAL_UI_INPUTS := $(wildcard $(TERMINAL_UI_DIR)/src/*.c \
	$(TERMINAL_UI_DIR)/include/gui/*.h)
IMAGEVIEWER_EXE := imageviewer/imageviewer.exe
FILEMANAGER_EXE := filemanager/filemanager.exe
$(TERMINAL_EXE) $(IMAGEVIEWER_EXE) $(FILEMANAGER_EXE): ../include/app/gui-limits.h
LIBPNG_INSTALL_DIR := $(ROOT)/libpng-1.6.59-install
ZLIB_INSTALL_DIR := $(ROOT)/zlib-1.3.2-install

.PHONY build:: build-terminal
build-terminal: $(TERMINAL_EXE)

.PHONY clean:: clean-terminal
clean-terminal:
	$(RM) $(TERMINAL_EXE) terminal/*.o terminal/*.d

# Rebuild the archive before linking apps whenever any UI implementation changes.
$(TERMINAL_UI_DIR)/libmario-ui.a: $(TERMINAL_UI_INPUTS)
	$(MAKE) -C $(TERMINAL_UI_DIR)

terminal/terminal.exe: terminal/main.c terminal/vt.c $(TERMINAL_UI_DIR)/libmario-ui.a
	$(CC) -o $@ terminal/main.c terminal/vt.c -I$(ROOT)/../include \
		-I$(TERMINAL_UI_DIR)/include \
		-ggdb $(OPTIMIZE) -fno-omit-frame-pointer \
		-L$(TERMINAL_UI_DIR) -L$(FREETYPE_INSTALL_DIR)/lib -static \
		-lmario-ui -lfreetype

.PHONY build:: build-imageviewer
build-imageviewer: $(IMAGEVIEWER_EXE)

.PHONY clean:: clean-imageviewer
clean-imageviewer:
	$(RM) $(IMAGEVIEWER_EXE) imageviewer/*.o imageviewer/*.d

imageviewer/imageviewer.exe: imageviewer/main.c $(TERMINAL_UI_DIR)/libmario-ui.a
	$(CC) -o $@ imageviewer/main.c -I$(ROOT)/../include \
		-I$(TERMINAL_UI_DIR)/include \
		-ggdb $(OPTIMIZE) -fno-omit-frame-pointer \
		-L$(TERMINAL_UI_DIR) -L$(LIBPNG_INSTALL_DIR)/lib \
		-L$(ZLIB_INSTALL_DIR)/lib -static -lmario-ui -lpng16 -lz

.PHONY build:: build-filemanager
build-filemanager: $(FILEMANAGER_EXE)

.PHONY clean:: clean-filemanager
clean-filemanager:
	$(RM) $(FILEMANAGER_EXE) filemanager/*.o filemanager/*.d

filemanager/filemanager.exe: filemanager/main.c filemanager/model.c \
	$(TERMINAL_UI_DIR)/libmario-ui.a
	$(CC) -o $@ filemanager/main.c filemanager/model.c -I$(ROOT)/../include \
		-I$(TERMINAL_UI_DIR)/include \
		-ggdb $(OPTIMIZE) -fno-omit-frame-pointer \
		-L$(TERMINAL_UI_DIR) -L$(FREETYPE_INSTALL_DIR)/lib -static \
		-lmario-ui -lfreetype
