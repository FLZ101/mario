SHELL := /bin/bash

.PHONY:	all clean

all: rdx

# Size of ramdisks are multiple of PAGE_SIZE (i.e. 4096B)
rdx: rd0 rd1 rd2
	cat rd0 rd1 rd2 >rdx

CFLAGS := -Wall -ggdb -Werror

TOOLS	:= rd-make.exe rd-explorer.exe

rd-make.exe: tools/rd-make.c tools/mariofs.h
	@$(CC) --mario-verify
	$(CC) -o $@ $(CFLAGS) $<

rd-explorer.exe: tools/rd-explorer.c tools/mariofs.h
	@$(CC) --mario-verify
	$(CC) -o $@ $(CFLAGS) $<

rd0: $(shell ./make-rd0.sh --deps)
	_LIBC=$(_LIBC) ./make-rd0.sh

rd1_files := $(shell find _ramdisk1 -type f)
rd1: $(TOOLS) $(rd1_files)
	./rd-make.exe --dir=_ramdisk1 --rd=rd1

rd2_files := $(shell find _ramdisk2 -type f)
rd2: $(TOOLS) $(rd2_files)
	./rd-make.exe --dir=_ramdisk2 --rd=rd2

clean:
	rm -f $(TOOLS)
	rm -rf ramdisk*/
	rm -f rd0 rd1 rd2 rdx
