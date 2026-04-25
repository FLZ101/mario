.PHONY:	all tools clean

all: rdx

# Size of ramdisks are multiple of PAGE_SIZE (i.e. 4096B)
rdx: rd0 rd1 rd2
	cat rd0 rd1 rd2 >rdx

tools:
	@$(CC) --mario-verify
	$(MAKE) -C rd-make
	$(MAKE) -C rd-explorer

EXES_EXAMPLE := $(wildcard ../app/example/*/*.exe)
EXES_TEST := $(wildcard ../app/test/*/*.exe)
ifeq ($(LIBC),musl)
  EXES_TEST	+= $(wildcard ../app/test/musl/*/*.exe)
endif

NCURSES_DIR	:= ../app/usr/ncurses-6.6-install
OKSH_DIR	:= ../app/usr/oksh-7.8-install
CMATRIX_DIR	:= ../app/usr/cmatrix-install
TOYBOX_DIR	:= ../app/usr/toybox-0.8.9-install

rd0_files := $(shell find ramdisk0 -type f)
rd0: tools $(EXES_EXAMPLE) $(EXES_TEST) \
	$(NCURSES_DIR)/share/terminfo/a/ansi \
	$(OKSH_DIR)/bin/oksh \
	$(rd0_files) device_list

	mkdir -p ramdisk0/{bin/example,bin/test,mnt,root/work,usr/share/terminfo}
	cp -u $(EXES_EXAMPLE) ramdisk0/bin/example
	cp -u $(EXES_TEST) ramdisk0/bin/test

	cp -u rd-make/main.c ramdisk0/root/work/rd-make.c

	# ncurses
	mkdir -p ramdisk0/usr/share/terminfo/{a,l,v,x}
	cp -u $(NCURSES_DIR)/share/terminfo/a/ansi ramdisk0/usr/share/terminfo/a
	cp -u $(NCURSES_DIR)/share/terminfo/l/linux ramdisk0/usr/share/terminfo/l
	cp -u $(NCURSES_DIR)/share/terminfo/v/vt100 ramdisk0/usr/share/terminfo/v
	cp -u $(NCURSES_DIR)/share/terminfo/x/xterm ramdisk0/usr/share/terminfo/x

	cp -u $(NCURSES_DIR)/bin/* ramdisk0/bin

	# oksh
	cp -u $(OKSH_DIR)/bin/oksh ramdisk0/bin

	# cmatrix
	cp -u $(CMATRIX_DIR)/cmatrix ramdisk0/bin

	# toybox
	cp -u --no-dereference $(TOYBOX_DIR)/bin/* ramdisk0/bin

	rd-make/rd-make.exe --dir=ramdisk0 --rd=rd0 --device-list=device_list --nr-block=81920

rd1_files := $(shell find ramdisk1 -type f)
rd1: tools $(rd1_files)
	rd-make/rd-make.exe --dir=ramdisk1 --rd=rd1

rd2_files := $(shell find ramdisk2 -type f)
rd2: tools $(rd2_files)
	rd-make/rd-make.exe --dir=ramdisk2 --rd=rd2

clean:
	$(MAKE) clean -C rd-make
	$(MAKE) clean -C rd-explorer
	rm -rf ramdisk0/{bin,mnt,usr,root}
	rm -f rd0 rd1 rd2 rdx
