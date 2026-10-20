SHELL := /bin/bash

.PHONY: all clean image update-image run debug quick-run quick-debug \
	tmux-run tmux-debug tmux-attach tmux-kill

all: image

GRUB_FILES	:= $(wildcard grub4dos-0.4.6a/*)

update-image: image
	bash update_image.sh

image: hd.img hd2.img hd3.img tmp/kernel.exe tmp/boot.bin tmp/rdx $(GRUB_FILES)

# gen_image.sh creates hd.img, hd2.img, and hd3.img, so use a grouped
# target (&:) to run the recipe a single time, producing all three files.
hd.img hd2.img hd3.img &: gen_image.sh
	bash gen_image.sh

tmp/kernel.exe: ../src/kernel/kernel.exe
	cp $^ $@

tmp/boot.bin: ../src/boot/boot.bin
	cp $^ $@

tmp/rdx: ../rd/rdx
	cp $^ $@

clean:
	find tmp -mindepth 1 -maxdepth 1 ! -name '.gitignore' -exec rm -rf {} +
	rm -f hd.img hd2.img hd3.img

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
	-drive id=disk,file=hd.img,format=raw,if=none \
	-drive id=disk2,file=hd2.img,format=raw,if=none \
	-drive id=disk3,file=hd3.img,format=raw,if=none \
	-device ahci,id=ahci \
	-device ide-hd,drive=disk,bus=ahci.0 \
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
	-kernel tmp/kernel.exe \
	-initrd tmp/rdx \
	-display $(QEMU_DISPLAY) \
	-no-reboot -no-shutdown

quick-run: tmp/kernel.exe tmp/rdx
	qemu-system-i386 $(QUICK_QEMU_FLAGS)

quick-debug: tmp/kernel.exe tmp/rdx
	qemu-system-i386 -s -S $(QUICK_QEMU_FLAGS)

tmux-run: tmp/kernel.exe tmp/rdx
	./tmux.sh start $(QUICK_QEMU_FLAGS)

tmux-debug: tmp/kernel.exe tmp/rdx
	./tmux.sh start -s -S $(QUICK_QEMU_FLAGS)

tmux-attach:
	./tmux.sh attach

tmux-kill:
	./tmux.sh kill
