SHELL := /bin/bash

# Number of QEMU CPUs.  Defaults to one until SMP bring-up lands so the
# existing single-CPU kernel path remains the default for all targets.
SMP ?= 1

.PHONY: all clean image run debug quick-run quick-debug \
	tmux-run tmux-debug tmux-attach tmux-kill

all: image

RDX := ../rd/rdx
KERNEL_EXE := ../src/kernel/kernel.exe

image: hd1.img hd2.img hd3.img

hd1.img: make-hd1.py mkdisk.py $(shell python3 make-hd1.py --deps)
	python3 make-hd1.py

hd2.img: make-hd2.py mkdisk.py $(shell python3 make-hd2.py --deps)
	python3 make-hd2.py

hd3.img: make-hd3.py mkdisk.py $(shell python3 make-hd3.py --deps)
	python3 make-hd3.py

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

HD_FLAGS := \
	-drive id=disk1,file=hd1.img,format=raw,if=none \
	-drive id=disk2,file=hd2.img,format=raw,if=none \
	-drive id=disk3,file=hd3.img,format=raw,if=none \
	-device ahci,id=ahci \
	-device ide-hd,drive=disk1,bus=ahci.0 \
	-device ide-hd,drive=disk2,bus=ahci.1 \
	-device ide-hd,drive=disk3,bus=ahci.2

QEMU_FLAGS := \
	-boot order=c \
	-m 256M \
	-smp $(SMP) \
	-display $(QEMU_DISPLAY) \
	$(HD_FLAGS) \
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
	-smp $(SMP) \
	-kernel $(KERNEL_EXE) \
	-initrd $(RDX) \
	-append "mario.smp=$(SMP)" \
	-display $(QEMU_DISPLAY) \
	$(HD_FLAGS) \
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
