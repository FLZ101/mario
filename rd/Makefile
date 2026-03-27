.PHONY:	all clean rd rd-make rd-explorer

all: rd

MESSAGE := $(shell $(CC) --mario-verify 2>&1)
ifneq ($(MESSAGE),)
  $(error Not a compiler for mario)
endif

rd: rd-make rd-explorer
	$(MAKE) rd0 rd1 rd2

rd-make:
	$(MAKE) -C rd-make

rd-explorer:
	$(MAKE) -C rd-explorer

EXES := $(wildcard ../app/*/*.exe)
EXES_TEST := $(wildcard ../app/test/*/*.exe)

rd0_files := $(shell find ramdisk0 -type f)
rd0: $(EXES) $(EXES_TEST) $(rd0_files) device_list
	mkdir -p ramdisk0/{bin/test,mnt,root}
	cp -u $(EXES) ramdisk0/bin
	cp -u $(EXES_TEST) ramdisk0/bin/test
	cp -u rd-make/main.c ramdisk0/root/rd-make.c
	rd-make/rd-make.exe --block-size=512 --device-list=device_list --dir=ramdisk0 --rd=rd0 --nr-block=8192

rd1_files := $(shell find ramdisk1 -type f)
rd1: $(rd1_files)
	rd-make/rd-make.exe --block-size=512 --dir=ramdisk1 --rd=rd1

rd2_files := $(shell find ramdisk2 -type f)
rd2: $(rd2_files)
	rd-make/rd-make.exe --block-size=512 --dir=ramdisk2 --rd=rd2

clean:
	$(MAKE) clean -C rd-make
	$(MAKE) clean -C rd-explorer
	rm -rf ramdisk0/{bin,mnt,root}
	rm -f rd0 rd1 rd2
