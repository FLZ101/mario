.PHONY: all clean image run debug quick-run quick-debug \
	tmux-run tmux-debug tmux-attach tmux-kill

all: image

GRUB_FILES	:= $(wildcard grub4dos-0.4.6a/*)

image: hd.img tmp/kernel.exe tmp/boot.bin tmp/rdx $(GRUB_FILES)
	bash update_image.sh

hd.img:
	bash gen_image.sh

tmp/kernel.exe: ../src/kernel/kernel.exe
	cp $^ $@

tmp/boot.bin: ../src/boot/boot.bin
	cp $^ $@

tmp/rdx: ../rd/rdx
	cp $^ $@

clean:
	find tmp -mindepth 1 -maxdepth 1 ! -name '.gitignore' -exec rm -rf {} +
	rm -f hd.img

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

QEMU_FLAGS := \
	-boot order=c \
	-m 256M \
	-drive file=hd.img,format=raw,index=0,media=disk \
	-display curses \
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
	-display curses \
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
