
.EXPORT_ALL_VARIABLES:

RM	:=rm -f
NASM	:=nasm
MAKE	:=make -R

TOOLCHAIN :=i686-w64-mingw32-
LD	:=$(TOOLCHAIN)ld -m i386pe
STRIP	:=$(TOOLCHAIN)strip
NM	:=$(TOOLCHAIN)nm
OBJDUMP	:=$(TOOLCHAIN)objdump
CC	:=$(TOOLCHAIN)gcc -m32

INCLUDE	:=-I include
CFLAGS	:=-c -D__KERNEL__ -nostdinc -ffreestanding $(INCLUDE) -g -Wall -O0 -fno-omit-frame-pointer -fno-stack-protector
LDFLAGS	:=-T kernel.ld -nostdlib --disable-reloc-section --image-base=0 --disable-dynamicbase --file-alignment=0x1000 --section-alignment=0x1000

.PHONY:	all depend clean

OBJS	:=$(addsuffix .o,$(basename $(wildcard */*.[Sc])))
OBJS	+=$(addsuffix .o,$(basename $(wildcard */*/*.[Sc])))
# make kernel/start.o the first object file
OBJS	:=$(filter-out start.o,$(OBJS))
OBJS	:=kernel/start.o $(OBJS)

DEPS	:=$(patsubst %.o,%.dep,$(OBJS))

ifeq (.depend,$(wildcard .depend))
all:
else
all: depend
endif
	@$(MAKE) -f Makeit
	@echo :\)

# `make depend' when files are added in
depend: $(DEPS)
	@touch .depend

clean:
	$(RM) $(OBJS)
	$(RM) $(DEPS) .depend
	$(RM) boot/boot.bin boot/boot.lst \
		kernel/kernel.exe kernel/kernel.dbg kernel/kernel.dbg.txt kernel/kernel.nm

%.dep: %.S
	@$(CC) -MM $(INCLUDE) -D__ASSEMBLY__ -o $@.tmp $<
	@sed -e 's,\($(basename $(notdir $<))\).o[ :]*,$(dir $<)\1.o $@: ,g' < $@.tmp > $@
	@$(RM) $@.tmp
	@echo $@

%.dep: %.c
	@$(CC) -MM $(INCLUDE) -o $@.tmp $<
	@sed -e 's,\($(basename $(notdir $<))\).o[ :]*,$(dir $<)\1.o $@: ,g' < $@.tmp > $@
	@$(RM) $@.tmp
	@echo $@
