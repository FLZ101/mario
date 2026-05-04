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

EXES_EXAMPLE := $(wildcard ../app/example/*/*.exe)
EXES_TEST := $(wildcard ../app/test/*/*.exe)
ifeq ($(LIBC),musl)
  EXES_TEST	+= $(wildcard ../app/test/musl/*/*.exe)
endif

NCURSES_DIR	:= ../app/usr/ncurses-6.6-install
NCURSES_PROGRAMS_DIR	:= ../app/usr/ncurses
OKSH_DIR	:= ../app/usr/oksh-7.8-install
TOYBOX_DIR	:= ../app/usr/toybox-0.8.9-install
SBASE_DIR	:= ../app/usr/sbase-install
UBASE_DIR	:= ../app/usr/ubase-install
VIM_DIR		:= ../app/usr/vim-9.2.0428-install
NANO_DIR		:= ../app/usr/nano-9.0-install

rd0_files := $(shell find ramdisk0 -type f)
rd0: $(TOOLS) $(EXES_EXAMPLE) $(EXES_TEST) \
	$(shell find $(NCURSES_DIR)/share/terminfo/) \
	$(shell find $(NCURSES_PROGRAMS_DIR) -name '*.exe') \
	$(OKSH_DIR)/bin/oksh \
	$(wildcard $(TOYBOX_DIR)/bin/toybox) \
	$(wildcard $(SBASE_DIR)/bin/sbase-box) \
	$(wildcard $(UBASE_DIR)/bin/ubase-box) \
	$(wildcard $(VIM_DIR)/bin/vim) \
	$(wildcard $(NANO_DIR)/bin/nano) \
	$(rd0_files) device_list

	mkdir -p ramdisk0/{bin/example,bin/test,mnt,root/work,usr/share/terminfo}
	cp -u $(EXES_EXAMPLE) ramdisk0/bin/example
	cp -u $(EXES_TEST) ramdisk0/bin/test

	cp -u tools/rd-make.c ramdisk0/root/work/

	# ncurses
	mkdir -p ramdisk0/usr/share/terminfo/{a,l,v,x}
	cp -u $(NCURSES_DIR)/share/terminfo/a/ansi ramdisk0/usr/share/terminfo/a
	cp -u $(NCURSES_DIR)/share/terminfo/l/linux ramdisk0/usr/share/terminfo/l
	cp -u $(NCURSES_DIR)/share/terminfo/v/{vt100,vt102} ramdisk0/usr/share/terminfo/v
	cp -u $(NCURSES_DIR)/share/terminfo/x/xterm ramdisk0/usr/share/terminfo/x

	cp -u $(NCURSES_DIR)/bin/* ramdisk0/bin

	# oksh
	cp -u $(OKSH_DIR)/bin/oksh ramdisk0/bin

	# ncurses programs
	mkdir -p ramdisk0/bin/ncurses
	cp -u $(NCURSES_PROGRAMS_DIR)/*.exe ramdisk0/bin/ncurses

# 	mkdir -p ramdisk0/bin/ncurses/guide
# 	cp -u $(NCURSES_PROGRAMS_DIR)/guide/*.exe ramdisk0/bin/ncurses/guide

	# toybox
	mkdir -p ramdisk0/bin/toybox
	cp -u --no-dereference $(TOYBOX_DIR)/bin/* ramdisk0/bin/toybox

	# sbase
	mkdir -p ramdisk0/bin/sbase
	cp -u --no-dereference $(SBASE_DIR)/bin/* ramdisk0/bin/sbase

	# ubase
	mkdir -p ramdisk0/bin/ubase
	cp -u --no-dereference $(UBASE_DIR)/bin/* ramdisk0/bin/ubase

	# vim
	mkdir -p ramdisk0/usr/share
	cp -u --no-dereference $(VIM_DIR)/bin/* ramdisk0/bin/
	cp -u -r $(VIM_DIR)/share/vim ramdisk0/usr/share

	# nano
	mkdir -p ramdisk0/usr/share
	cp -u --no-dereference $(NANO_DIR)/bin/* ramdisk0/bin/
	cp -u -r $(NANO_DIR)/share/nano ramdisk0/usr/share

	# strip executables
	chmod +w ramdisk0/bin/toybox/toybox
	chmod -x ramdisk0/bin/ncursesw_g6-config
	for x in $$(find ramdisk0/bin/ -type f -executable) ; do \
		strip --strip-all $$x ; \
	done

	./rd-make.exe --dir=ramdisk0 --rd=rd0 --device-list=device_list --nr-block=81920

rd1_files := $(shell find ramdisk1 -type f)
rd1: $(TOOLS) $(rd1_files)
	./rd-make.exe --dir=ramdisk1 --rd=rd1

rd2_files := $(shell find ramdisk2 -type f)
rd2: $(TOOLS) $(rd2_files)
	./rd-make.exe --dir=ramdisk2 --rd=rd2

clean:
	rm -f $(TOOLS)
	rm -rf ramdisk0/{bin,mnt,usr,root}
	rm -f rd0 rd1 rd2 rdx
