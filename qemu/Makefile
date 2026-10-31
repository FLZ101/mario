SHELL := /bin/bash

# Number of QEMU CPUs.  Defaults to one until SMP bring-up lands so the
# existing single-CPU kernel path remains the default for all targets.
SMP ?= 1
# grub4dos menu.lst entry to boot by default.  Use BOOT_ENTRY=N when
# running `make run`, `make debug`, or `make image` from this directory
# (or the top level) to preselect that numbered menu entry and boot with
# no menu timeout.  Leave BOOT_ENTRY unset (internally x) to use the
# original menu.lst unchanged.
BOOT_ENTRY ?= x

.PHONY: all clean image run debug quick-run quick-debug \
	tmux-run tmux-debug tmux-attach tmux-kill

all: image

RDX := ../rd/rdx
KERNEL_EXE := ../src/kernel/kernel.exe

image: hd1.img hd2.img hd3.img

ifeq ($(BOOT_ENTRY),x)
BOOT_MENU := grub4dos-0.4.6a/menu.lst
HD1_MENU_ENV :=
else
BOOT_MENU := tmp/menu-$(BOOT_ENTRY).lst
HD1_MENU_ENV := MARIO_BOOT_MENU=$(BOOT_MENU)
endif

ifneq ($(BOOT_ENTRY),x)
$(BOOT_MENU): grub4dos-0.4.6a/menu.lst
	@mkdir -p tmp
	@awk -v d='$(BOOT_ENTRY)' \
		'BEGIN { printf "default %s\ntimeout 0\n", d } \
		 !/^default[ \t]/ && !/^timeout[ \t]/ { print }' $< > $@
endif

HD1_BOOT_STAMP := tmp/.hd1-menu-$(BOOT_ENTRY)

$(HD1_BOOT_STAMP): $(BOOT_MENU)
	@mkdir -p tmp
	@rm -f tmp/.hd1-menu-*
	@touch $@

hd1.img: make-hd1.py mkdisk.py $(HD1_BOOT_STAMP) $(shell $(HD1_MENU_ENV) python3 make-hd1.py --deps)
	$(HD1_MENU_ENV) python3 make-hd1.py

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
	-snapshot \
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
