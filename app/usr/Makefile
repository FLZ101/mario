RM	:=rm -f

ROOT := $(shell pwd)

.PHONY:: all build
.PHONY:: clean clean-src

all:: build

include ncurses.mk
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
		-ggdb -O0 -fno-omit-frame-pointer \
		-L$(NCURSES_INSTALL_DIR)/lib -static \
		-lncurses

include vim.mk
include nano.mk
