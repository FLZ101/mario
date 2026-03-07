.PHONY:	all clean rd rd-make rd-explorer

all: rd

rd: rd-make rd-explorer
	$(MAKE) rd0 rd1 rd2

rd-make:
	$(MAKE) -C rd-make

rd-explorer:
	$(MAKE) -C rd-explorer

EXES := $(wildcard ../app/**/*.exe)
ROOT_FILES := ../app/kilo/main.c

rd0_files := $(shell find ramdisk0 -type f)
rd0: $(EXES) $(rd0_files)
	cp -u $(EXES) ramdisk0/bin
	cp -u $(ROOT_FILES) ramdisk0/root
	rd-make/rd-make.exe --block-size=512 --device-list=device_list --dir=ramdisk0 --rd=rd0 \
		--nr-block=8192

rd1_files := $(shell find ramdisk1 -type f)
rd1: $(rd1_files)
	rd-make/rd-make.exe --block-size=512 --dir=ramdisk1 --rd=rd1

rd2_files := $(shell find ramdisk2 -type f)
rd2: $(rd2_files)
	rd-make/rd-make.exe --block-size=512 --dir=ramdisk2 --rd=rd2

clean:
	$(MAKE) clean -C rd-make
	$(MAKE) clean -C rd-explorer
	find ramdisk0/bin -mindepth 1 -maxdepth 1 ! -name '.gitignore' -exec rm -r {} +
	rm -f rd0 rd1 rd2
