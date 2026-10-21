SHELL := /bin/bash

.PHONY: all clean image run debug quick-run quick-debug \
	tmux-run tmux-debug tmux-attach tmux-kill

all: image

RDX := ../rd/rdx
KERNEL_EXE := ../src/kernel/kernel.exe

image: hd1.img hd2.img hd3.img

hd1.img: $(shell ./make-hd1.sh --deps)
	bash make-hd1.sh

hd2.img: $(shell ./make-hd2.sh --deps)
	bash make-hd2.sh

hd3.img: $(shell ./make-hd3.sh --deps)
	bash make-hd3.sh

clean:
	rm -rf tmp/ hd1.img hd2.img hd3.img

#	-display gtk
#
#     	https://www.qemu.org/docs/master/system/keys.html
#
#       Ctrl-Alt-1
#       Ctrl-Alt-2
#
#	-display curses
#
#	    Alt-1
#       Alt-2
#
#	-nographic
#
#		https://www.qemu.org/docs/master/system/mux-chardev.html
#
#		Ctrl+a x

ifneq ($(QEMU_DISPLAY),gtk)
QEMU_DISPLAY	:=curses
endif

QEMU_FLAGS := \
	-boot order=c \
	-m 256M \
	-drive id=disk1,file=hd1.img,format=raw,if=none \
	-drive id=disk2,file=hd2.img,format=raw,if=none \
	-drive id=disk3,file=hd3.img,format=raw,if=none \
	-device ahci,id=ahci \
	-device ide-hd,drive=disk1,bus=ahci.0 \
	-device ide-hd,drive=disk2,bus=ahci.1 \
	-device ide-hd,drive=disk3,bus=ahci.2 \
	-display $(QEMU_DISPLAY) \
	-no-reboot -no-shutdown

run:
	qemu-system-i386 $(QEMU_FLAGS)

debug:
	qemu-system-i386 -s -S $(QEMU_FLAGS)

#	-display gtk
#	-display curses
#	-d int,mmu,guest_errors -D qemu.log
QUICK_QEMU_FLAGS := \
	-m 256M \
	-kernel $(KERNEL_EXE) \
	-initrd $(RDX) \
	-display $(QEMU_DISPLAY) \
	-no-reboot -no-shutdown

quick-run:
	qemu-system-i386 $(QUICK_QEMU_FLAGS)

quick-debug:
	qemu-system-i386 -s -S $(QUICK_QEMU_FLAGS)

tmux-run:
	./tmux.sh start $(QUICK_QEMU_FLAGS)

tmux-debug:
	./tmux.sh start -s -S $(QUICK_QEMU_FLAGS)

tmux-attach:
	./tmux.sh attach

tmux-kill:
	./tmux.sh kill
