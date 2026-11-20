SHELL := /bin/bash

.PHONY:	all clean

all: rdx

# Size of ramdisks are multiple of PAGE_SIZE (i.e. 4096B)
rdx: rd0 rd1 rd2
	cat rd0 rd1 rd2 >rdx

rdx-gtk: rd0-gtk rd1 rd2 ramdisk0_usr-gtk
	cat rd0-gtk rd1 rd2 >$@

CFLAGS := -Wall -ggdb -Werror

TOOLS	:= rd-make.exe rd-explorer.exe
PYTHON	?= python3

rd-make.exe: tools/rd-make.c tools/mariofs.h
	@$(CC) --mario-verify
	$(CC) -o $@ $(CFLAGS) $<

rd-explorer.exe: tools/rd-explorer.c tools/mariofs.h
	@$(CC) --mario-verify
	$(CC) -o $@ $(CFLAGS) $<

rd0: $(TOOLS) device_list make-rd0.py copy_files.py $(shell $(PYTHON) make-rd0.py --deps)
	$(PYTHON) make-rd0.py

gtk_overlay_stamp := ramdisk0-gtk-deps/$(shell \
	if test -d _ramdisk0-gtk; then find _ramdisk0-gtk -printf '%C@ %p\0'; fi \
	| LC_ALL=C sort -z | sha256sum | cut -d' ' -f1)

$(gtk_overlay_stamp):
	mkdir -p $(@D)
	rm -f $(@D)/*
	touch $@

rd0-gtk ramdisk0_usr-gtk &: rd0 $(gtk_overlay_stamp)
	$(PYTHON) make-rd0.py --gtk

rd1_files := $(shell find _ramdisk1 -type f)
rd1: $(TOOLS) $(rd1_files)
	./rd-make.exe --dir=_ramdisk1 --rd=rd1

rd2_files := $(shell find _ramdisk2 -type f)
rd2: $(TOOLS) $(rd2_files)
	./rd-make.exe --dir=_ramdisk2 --rd=rd2

clean:
	rm -f $(TOOLS)
	rm -rf ramdisk*/
	rm -f rd0 rd0-gtk rd1 rd2 rdx rdx-gtk
