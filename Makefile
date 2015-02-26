
.EXPORT_ALL_VARIABLES:

RM      :=rm -f
NASM    :=nasm
MAKE    :=mingw32-make -R
LD      :=ld -m i386pe
LDFLAGS :=-T Link.ld -nostdlib
STRIP   :=strip
NM      :=nm
CC      :=gcc -m32
INCLUDE :=-I include
CFLAGS  :=-c -nostdinc -ffreestanding $(INCLUDE) -ggdb -Wall -O0

.PHONY: all depend clean clean-all

OBJS    :=$(addsuffix .o,$(basename $(wildcard */*.[Sc])))

DEPS    :=$(patsubst %.o,%.dep,$(OBJS))

ifeq (.depend,$(wildcard .depend))
all:
else
all: depend
endif
	@$(MAKE) -f Makeit
	@echo \:\)

# `make depend' when file(s) added in
depend: $(DEPS)
	@touch .depend

clean:	OBJS:=$(wildcard */*.o)
clean:	DEPS:=$(wildcard */*.dep)
clean:
	$(RM) $(OBJS) $(DEPS) .depend

clean-all: clean
	$(RM) boot/boot.bin boot/boot.lst \
	kernel/kernel.exe kernel/kernel.dbg kernel/kernel.nm

%.dep: %.S
	@$(CC) -MM $(INCLUDE) -D__ASSEMBLY__ -o $@.tmp $<
	@sed -e 's,\($(basename $(notdir $<))\).o[ :]*,$(dir $<)\1.o $@: ,g' < $@.tmp > $@
	@$(RM) $@.tmp
	@cat $@

%.dep: %.c
	@$(CC) -MM $(INCLUDE) -o $@.tmp $<
	@sed -e 's,\($(basename $(notdir $<))\).o[ :]*,$(dir $<)\1.o $@: ,g' < $@.tmp > $@ 
	@$(RM) $@.tmp
	@cat $@
