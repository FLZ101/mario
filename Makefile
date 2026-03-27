RM	:=rm -f

ROOT := $(shell pwd)

.PHONY: all kernel rd app libc image run debug quick-run quick-debug

.PHONY: clean clean-kernel clean-rd clean-app clean-libc clean-image

all: kernel rd

COMPILE_COMMANDS_JSON := $(ROOT)/compile_commands.json
export COMPILE_COMMANDS_JSON

CC	:= $(ROOT)/util/cc
export CC
LD	:= $(ROOT)/util/ld
export LD

kernel:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -C src

rd: app
	$(MAKE) -C rd

app: libc
	$(MAKE) CC=$(ROOT)/util/mario-libc-cc LD=$(ROOT)/util/mario-libc-ld -C app

libc:
	$(MAKE) CC=$(ROOT)/util/mario-cc -C libc

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

clean-image:
	$(MAKE) clean -C qemu

.PHONY: tmux-run tmux-debug tmux-attach tmux-kill
tmux-run tmux-debug tmux-attach tmux-kill:
	$(MAKE) $@ -C qemu
