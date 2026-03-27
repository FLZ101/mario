RM	:=rm -f
NASM	:=nasm
MAKE	:=make -R

STRIP	:=strip
NM	:=nm
OBJDUMP	:=objdump

INCLUDE	:=-I include
CFLAGS	:=-c -D__KERNEL__ $(INCLUDE) \
	-std=gnu11 -Wall -Werror \
	-ggdb -O0 -fno-omit-frame-pointer \
	-MMD -MP
LDFLAGS	:=-T kernel.ld

.PHONY:	all clean

MESSAGE := $(shell $(CC) --mario-verify 2>&1)
ifneq ($(MESSAGE),)
  $(error Not a compiler for mario)
endif
MESSAGE := $(shell $(LD) --mario-verify 2>&1)
ifneq ($(MESSAGE),)
  $(error Not a linker for mario)
endif

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
