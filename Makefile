
.PHONY: all kernel rd app libc image run debug

.PHONY: clean clean-kernel clean-rd clean-app clean-libc clean-image

all: kernel

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

run:
	$(MAKE) run -C qemu

debug:
	$(MAKE) debug -C qemu

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
