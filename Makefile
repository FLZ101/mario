RM	:= rm -f

ROOT := $(shell pwd)

.PHONY: all kernel rd app libc image run debug \
	quick-run quick-debug quick-run-gtk quick-debug-gtk musl

.PHONY: clean clean-kernel clean-rd clean-app clean-libc clean-image clean-musl

all: kernel rd

COMPILE_COMMANDS_JSON := $(ROOT)/compile_commands.json
export COMPILE_COMMANDS_JSON

CC	:= $(ROOT)/util/cc
export CC
LD	:= $(ROOT)/util/ld
export LD

_LIBC	:= musl
# _LIBC	:= libc
export _LIBC

ifeq ($(_LIBC), libc)
  APP_CC	:= $(ROOT)/util/mario-libc-cc
  APP_LD	:= $(ROOT)/util/mario-libc-ld
else
  APP_CC	:= $(ROOT)/util/mario-musl-cc
  APP_LD	:= $(ROOT)/util/mario-musl-ld
endif

kernel:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -C src

rd: app
	$(MAKE) -C rd

app: $(_LIBC)
	$(MAKE) CC=$(APP_CC) LD=$(APP_LD) -C app

libc:
	$(MAKE) CC=$(ROOT)/util/mario-cc -C libc

musl:
	$(MAKE) CC=$(ROOT)/util/mario-cc COMPILE_COMMANDS_JSON= -C musl

image: kernel rd
	$(MAKE) -C qemu

run:
	$(MAKE) run -C qemu

debug:
	$(MAKE) debug -C qemu

quick-run:
	$(MAKE) quick-run -C qemu

quick-debug:
	$(MAKE) quick-debug -C qemu

quick-run-gtk:
	$(MAKE) QEMU_DISPLAY=gtk quick-run -C qemu

quick-debug-gtk:
	$(MAKE) QEMU_DISPLAY=gtk quick-debug -C qemu

clean: clean-kernel clean-rd clean-app clean-libc clean-image
	$(RM) $(COMPILE_COMMANDS_JSON)

clean-kernel:
	$(MAKE) clean -C src

clean-rd:
	$(MAKE) clean -C rd

clean-app:
	$(MAKE) clean -C app

clean-libc:
	$(MAKE) clean -C libc

clean-musl:
	$(MAKE) clean -C musl

clean-image:
	$(MAKE) clean -C qemu

.PHONY: tmux-run tmux-debug tmux-attach tmux-kill
tmux-run tmux-debug tmux-attach tmux-kill:
	$(MAKE) $@ QEMU_DISPLAY=curses -C qemu
