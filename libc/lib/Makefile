.EXPORT_ALL_VARIABLES:

.PHONY: all clean

TOOLCHAIN :=i686-w64-mingw32-
RM :=rm -f
CP :=cp
CC :=$(TOOLCHAIN)gcc
INCLUDE :=-I$(LIBC)/../include
CFLAGS :=-m32 -c -nostdinc -ffreestanding $(INCLUDE) -ggdb
LD :=$(TOOLCHAIN)ld
LDFLAGS :=-m i386pe -L$(LIBC) -T Link.ld -nostdlib -lc \
	--image-base 0x00400000 --section-alignment 0x1000 --file-alignment 0x1000
OBJDUMP :=$(TOOLCHAIN)objdump

$(EXE): $(OBJS)
	$(LD) -o $@ $^ $(LDFLAGS)
	$(OBJDUMP) -d -S -M i386 $@ > $@.dbg.txt

%.o: %.S
	@$(CC) $(CFLAGS) -D__ASSEMBLY__ -o $@ $<

%.o: %.s
	@$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	@$(CC) $(CFLAGS) -o $@ $<
