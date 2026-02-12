.PHONY: all image run debug quick-run quick-debug clean

all: image

image: hd.img tmp/kernel.exe tmp/boot.bin tmp/rd0 tmp/rd1 tmp/rd2
	bash update_image.sh

hd.img:
	bash gen_image.sh

tmp/kernel.exe: ../src/kernel/kernel.exe
	cp $^ $@

tmp/boot.bin: ../src/boot/boot.bin
	cp $^ $@

tmp/rd0: ../rd/rd0
	cp $^ $@

tmp/rd1: ../rd/rd1
	cp $^ $@

tmp/rd2: ../rd/rd2
	cp $^ $@

clean:
	find tmp -mindepth 1 -maxdepth 1 ! -name '.gitignore' -exec rm -rf {} +
	rm -f hd.img

#	-display gtk
#
#	-display curses
#
#	  https://www.qemu.org/docs/master/system/keys.html
#	    `Ctrl+a x` won't work
#
#	  https://www.qemu.org/docs/master/system/keys.html
#	    Type `Alt+2` to switch to monitor tab, and type `Alt+1` to switch back.
#	    Make sure the keys are not handled by the terminal emulator (e.g. Windows Terminal, Kconsole)

QEMU_FLAGS := \
	-boot order=c \
	-m 256M \
	-drive file=hd.img,format=raw,index=0,media=disk \
	-display curses

run:
	qemu-system-i386 $(QEMU_FLAGS)

debug:
	qemu-system-i386 -s -S $(QEMU_FLAGS)

#	-display gtk
#	-display curses
QUICK_QEMU_FLAGS := \
	-m 256M \
	-kernel tmp/kernel.exe \
	-initrd tmp/rd0 \
	-display gtk

quick-run: tmp/kernel.exe tmp/rd0
	qemu-system-i386 $(QUICK_QEMU_FLAGS)

quick-debug: tmp/kernel.exe tmp/rd0
	qemu-system-i386 -s -S $(QUICK_QEMU_FLAGS)
