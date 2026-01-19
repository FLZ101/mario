.EXPORT_ALL_VARIABLES:

.PHONY: all clean

TOOLCHAIN :=
RM :=rm -f
CP :=cp
CC :=$(TOOLCHAIN)gcc -m32
INCLUDE :=-I$(LIBC)/../include
CFLAGS	:=-c $(INCLUDE) \
	-nostdinc -ffreestanding \
	-fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ggdb -Wall -O0 -fno-omit-frame-pointer -fno-stack-protector
LD :=$(TOOLCHAIN)ld -m elf_i386
LDFLAGS :=-L$(LIBC) -T Link.ld -nostdlib -lc
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
