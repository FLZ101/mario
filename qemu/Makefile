.PHONY: all image run debug clean

all: image

image: hd.img tmp/kernel.exe tmp/boot.bin tmp/rd0 tmp/rd1 tmp/rd2
	bash update_image.sh

hd.img:
	bash gen_image.sh

tmp/kernel.exe: ../src/kernel/kernel.exe
	cp $^ $@

tmp/boot.bin: ../src/boot/boot.bin
	cp $^ $@

tmp/rd0: ../rd/rd0
	cp $^ $@

tmp/rd1: ../rd/rd1
	cp $^ $@

tmp/rd2: ../rd/rd2
	cp $^ $@

clean:
	find tmp -mindepth 1 -maxdepth 1 ! -name '.gitignore' -exec rm -rf {} +
	rm -f hd.img

QEMU_FLAGS := \
	-boot order=c \
	-m 256M \
	-drive file=hd.img,format=raw,index=0,media=disk \
	-display gtk

run:
	qemu-system-i386 $(QEMU_FLAGS)

debug:
	qemu-system-i386 -s -S $(QEMU_FLAGS)
