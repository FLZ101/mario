
.EXPORT_ALL_VARIABLES:

RM	:=rm -f
NASM	:=nasm
MAKE	:=make -R

TOOLCHAIN :=
LD	:=$(TOOLCHAIN)ld -m elf_i386
STRIP	:=$(TOOLCHAIN)strip
NM	:=$(TOOLCHAIN)nm
OBJDUMP	:=$(TOOLCHAIN)objdump
CC	:=$(TOOLCHAIN)gcc -m32

INCLUDE	:=-I include
CFLAGS	:=-c -D__KERNEL__ $(INCLUDE) \
	-std=gnu11 -Wall -Werror \
	-nostdinc -ffreestanding \
	-fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ggdb -O0 -fno-omit-frame-pointer -fno-stack-protector \
	-MMD -MP
LDFLAGS	:=-T kernel.ld --orphan-handling=warn -nostdlib

.PHONY:	all clean

OBJS	:=$(addsuffix .o,$(basename $(wildcard */*.[Sc])))
OBJS	+=$(addsuffix .o,$(basename $(wildcard */*/*.[Sc])))
# make kernel/start.o the first object file
OBJS	:=$(filter-out start.o,$(OBJS))
OBJS	:=kernel/start.o $(OBJS)

DEPS	:=$(patsubst %.o,%.d,$(OBJS))

all: boot/boot.bin kernel/kernel.exe

clean:
	$(RM) $(OBJS)
	$(RM) $(DEPS)
	$(RM) boot/boot.bin boot/boot.lst kernel/kernel.exe

-include $(DEPS)

%.bin: %.asm
	@$(NASM) -f bin -l $*.lst -o $@ $<
	@echo $@

kernel/kernel.exe: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.S
	@$(CC) $(CFLAGS) -D__ASSEMBLY__ -o $@ $<
	@echo $@

%.o: %.c
	@$(CC) $(CFLAGS) -o $@ $<
	@echo $@
