RM	:=rm -f

ROOT := $(shell pwd)

.PHONY:: all build
.PHONY:: clean clean-src

all:: build

include ncurses.mk
include libedit.mk
include dash.mk
include oksh.mk
