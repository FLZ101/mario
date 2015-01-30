
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
CFLAGS  :=-c -nostdinc -ffreestanding $(INCLUDE) -ggdb -Wall -O1

.PHONY: all depend clean clean-all

OBJS    :=$(addsuffix .o,$(basename $(wildcard */*.[Sc])))
# Make kernel/start.o the 1st object file
OBJS    :=$(filter-out kernel/start.o,$(OBJS))
OBJS    :=kernel/start.o $(OBJS)

DEPS    :=$(patsubst %.o,%.dep,$(OBJS))

ifeq (.depend,$(wildcard .depend))
all:
	@$(MAKE) -f Makeit
	@echo \:\)
else
all: depend
	@$(MAKE) -f Makeit
	@echo \:\)
endif

# When file(s) added in, deleted or renamed, `make depend'
depend: $(DEPS)
	@touch .depend

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
