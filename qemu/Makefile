SHELL := /bin/bash

PYTHON ?= python3

# Number of QEMU CPUs.  Defaults to one until SMP bring-up lands so the
# existing single-CPU kernel path remains the default for all targets.
SMP ?= 1
# grub4dos menu file installed as /menu.lst.  Prebuilt menus live in
# qemu/grub4dos-0.4.6a.
BOOT_MENU ?= menu.lst
BOOT_MENU_FILE := grub4dos-0.4.6a/$(BOOT_MENU)

.PHONY: all clean image image-gtk run debug quick-run quick-debug \
	tmux-run tmux-debug tmux-attach tmux-kill

all: image

RDX := ../rd/rdx
KERNEL_EXE := ../src/kernel/kernel.exe

image: hd1.img hd2.img hd3.img
image-gtk: hd1-gtk.img hd2.img hd3-gtk.img

HD1_BOOT_STAMP := tmp/.hd1-boot-$(BOOT_MENU)

$(HD1_BOOT_STAMP): $(BOOT_MENU_FILE)
	@mkdir -p tmp
	@rm -f tmp/.hd1-boot-*
	@touch $@

HD1_ENV := MARIO_HD1_GTK=0 MARIO_BOOT_MENU='$(BOOT_MENU_FILE)'
HD1_GTK_ENV := MARIO_HD1_GTK=1 MARIO_BOOT_MENU=grub4dos-0.4.6a/menu.lst

hd1.img: make-hd1.py mkdisk.py $(HD1_BOOT_STAMP) $(shell $(HD1_ENV) $(PYTHON) make-hd1.py --deps)
	$(HD1_ENV) $(PYTHON) make-hd1.py

hd1-gtk.img: make-hd1.py mkdisk.py $(shell $(HD1_GTK_ENV) $(PYTHON) make-hd1.py --deps)
	$(HD1_GTK_ENV) $(PYTHON) make-hd1.py

hd2.img: make-hd2.py mkdisk.py $(shell $(PYTHON) make-hd2.py --deps)
	$(PYTHON) make-hd2.py

HD3_DEPS := $(shell MARIO_HD3_GTK=0 $(PYTHON) make-hd3.py --deps)

hd3.img: make-hd3.py mkdisk.py $(HD3_DEPS)
	MARIO_HD3_GTK=0 $(PYTHON) make-hd3.py

hd3-gtk.img: make-hd3.py mkdisk.py $(filter-out ../rd/ramdisk0_usr%,$(HD3_DEPS)) ../rd/ramdisk0_usr-gtk
	MARIO_HD3_GTK=1 $(PYTHON) make-hd3.py

clean:
	rm -rf tmp/ hd1.img hd1-gtk.img hd2.img hd3.img hd3-gtk.img

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
QEMU_VMPORT := off
else
QEMU_VMPORT := on
endif

BOOT_DISK ?= hd1.img
USR_DISK ?= hd3.img
QEMU_AUDIO ?= $(if $(PULSE_SERVER),pa)
QEMU_NET ?= off
QEMU_LAUNCH := qemu-system-i386

ifeq ($(QEMU_NET),nat)
QEMU_SSH_PORT ?= auto
endif

ifneq ($(QEMU_SSH_PORT),)
ifneq ($(QEMU_NET),nat)
$(error QEMU_SSH_PORT requires QEMU_NET=nat)
endif
ifneq ($(QEMU_SSH_PORT),auto)
ifneq ($(shell port='$(subst ','"'"',$(QEMU_SSH_PORT))'; \
	[[ $$port =~ ^[1-9][0-9]{0,4}$$ ]] && (( port <= 65535 )) && printf yes),yes)
$(error QEMU_SSH_PORT must be auto or a TCP port from 1 to 65535)
endif
endif
QEMU_LAUNCH := $(PYTHON) ./run-with-ssh.py $(QEMU_SSH_PORT) $(QEMU_LAUNCH)
endif

ifeq ($(QEMU_NET),off)
NETWORK_FLAGS := -nic none
else ifeq ($(QEMU_NET),isolated)
NETWORK_FLAGS := -nic user,model=e1000,restrict=on
else ifeq ($(QEMU_NET),nat)
NETWORK_FLAGS := -nic user,model=e1000,ipv6=off,restrict=off
else
$(error Unsupported QEMU_NET '$(QEMU_NET)'; use off, isolated, or nat)
endif

HD_FLAGS := \
	-snapshot \
	-drive id=disk1,file=$(BOOT_DISK),format=raw,if=none \
	-drive id=disk2,file=hd2.img,format=raw,if=none \
	-drive id=disk3,file=$(USR_DISK),format=raw,if=none \
	-device ahci,id=ahci \
	-device ide-hd,drive=disk1,bus=ahci.0 \
	-device ide-hd,drive=disk2,bus=ahci.1 \
	-device ide-hd,drive=disk3,bus=ahci.2

AUDIO_FLAGS := \
	$(if $(QEMU_AUDIO),-audio "$(QEMU_AUDIO)") \
	-device ich9-intel-hda,id=mario-hda,msi=off \
	-device hda-output,bus=mario-hda.0

QEMU_FLAGS := \
	-boot order=c \
	-m 256M \
	-smp $(SMP) \
	-cpu max \
	-display $(QEMU_DISPLAY) \
	-machine vmport=$(QEMU_VMPORT) \
	$(NETWORK_FLAGS) \
	$(AUDIO_FLAGS) \
	$(HD_FLAGS)

QEMU_FLAGS_DEBUG := $(QEMU_FLAGS) -no-reboot -no-shutdown

run:
	$(QEMU_LAUNCH) $(QEMU_FLAGS)

debug:
	$(QEMU_LAUNCH) -s -S $(QEMU_FLAGS_DEBUG)

#	-display gtk
#	-display curses
#	-d int,mmu,guest_errors -D qemu.log
QUICK_QEMU_FLAGS := \
	-m 256M \
	-smp $(SMP) \
	-cpu max \
	-kernel $(KERNEL_EXE) \
	-initrd $(RDX) \
	-display $(QEMU_DISPLAY) \
	-machine vmport=$(QEMU_VMPORT) \
	$(NETWORK_FLAGS) \
	$(AUDIO_FLAGS) \
	$(HD_FLAGS)

QUICK_QEMU_FLAGS_DEBUG := $(QUICK_QEMU_FLAGS) -no-reboot -no-shutdown

quick-run:
	$(QEMU_LAUNCH) $(QUICK_QEMU_FLAGS)

quick-debug:
	$(QEMU_LAUNCH) -s -S $(QUICK_QEMU_FLAGS_DEBUG)

tmux-run:
	./tmux.sh start $(QEMU_LAUNCH) $(QUICK_QEMU_FLAGS)

tmux-debug:
	./tmux.sh start $(QEMU_LAUNCH) -s -S $(QUICK_QEMU_FLAGS_DEBUG)

tmux-attach:
	./tmux.sh attach

tmux-kill:
	./tmux.sh kill
