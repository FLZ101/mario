RM	:=rm -f

.PHONY: all kernel rd app libc image compile_db run debug quick-run quick-debug

.PHONY: clean clean-kernel clean-rd clean-app clean-libc clean-image clean-compile_db

all: kernel rd

kernel:
	$(MAKE) -C src

rd: app
	$(MAKE) -C rd

app: libc
	$(MAKE) -C app

libc:
	$(MAKE) -C libc

image: kernel rd
	$(MAKE) -C qemu

compile_db:
	compiledb $(MAKE) kernel libc app

run:
	$(MAKE) run -C qemu

debug:
	$(MAKE) debug -C qemu

quick-run:
	$(MAKE) quick-run -C qemu

quick-debug:
	$(MAKE) quick-debug -C qemu

clean: clean-kernel clean-rd clean-app clean-libc clean-image

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

clean-compile_db:
	$(RM) compile_commands.json
