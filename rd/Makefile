.PHONY:	all clean rd rd-make rd-explorer

all: rd

rd: rd-make rd-explorer
	$(MAKE) rd0 rd1 rd2

rd-make:
	$(MAKE) -C rd-make

rd-explorer:
	$(MAKE) -C rd-explorer

BINS := ../app/init/init.exe \
	../app/test_exec/test_exec.exe \
	../app/test_fork/test_fork.exe \
	../app/test_signal/test_signal.exe

rd0: $(BINS)
	cp -u $(BINS) ramdisk0/bin
	rd-make/rd-make.exe --block-size=512 --device-list=device_list --dir=ramdisk0 --rd=rd0

rd1:
	rd-make/rd-make.exe --block-size=512 --dir=ramdisk1 --rd=rd1

rd2:
	rd-make/rd-make.exe --block-size=512 --dir=ramdisk2 --rd=rd2

clean:
	$(MAKE) clean -C rd-make
	$(MAKE) clean -C rd-explorer
	find ramdisk0/bin -mindepth 1 -maxdepth 1 ! -name '.gitignore' -exec rm -r {} +
	rm -f rd0 rd1 rd2
