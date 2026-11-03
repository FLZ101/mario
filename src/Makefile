SHELL := /bin/bash

RM	:=rm -f
OPTIMIZE ?= -O0
AUTO_VAR_INIT ?= 1
KMALLOC_DEBUG ?= 1
PAGE_ALLOC_DEBUG ?= 1
STACK_PROTECTOR ?= 1
UBSAN ?= 0
NASM	:=nasm
MAKE	:=make -R

STRIP	:=strip
NM	:=nm
OBJDUMP	:=objdump

INCLUDE	:=-I include
CFLAGS	:=-c -D__KERNEL__ $(INCLUDE) \
	-std=gnu11 -Wall -Werror \
	-ggdb $(OPTIMIZE) -fno-omit-frame-pointer \
	-MMD -MP

WARN_FLAGS ?= -Wmaybe-uninitialized -Warray-bounds=2 -Wstringop-overflow=4
CFLAGS += $(WARN_FLAGS)

ifeq ($(AUTO_VAR_INIT),1)
CFLAGS	+= -ftrivial-auto-var-init=pattern
endif
ifeq ($(KMALLOC_DEBUG),1)
CFLAGS	+= -DCONFIG_KMALLOC_DEBUG
endif
ifeq ($(PAGE_ALLOC_DEBUG),1)
CFLAGS	+= -DCONFIG_PAGE_ALLOC_DEBUG
endif
ifeq ($(UBSAN),1)
UBSAN_FLAGS ?= -fsanitize=bounds,object-size,pointer-overflow,shift
UBSAN_FLAGS += -fno-sanitize-recover=undefined
CFLAGS	+= $(UBSAN_FLAGS) -DCONFIG_UBSAN
endif
LDFLAGS	:=-T kernel.ld

.PHONY:	all clean FORCE

GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

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
OBJS	+=kernel/smp_trampoline.o

DEPS	:=$(patsubst %.o,%.d,$(OBJS))

all: boot/boot.bin kernel/kernel.exe

$(OBJS): | include/build_info.h

include/build_info.h: FORCE
	@mkdir -p $(dir $@)
	@printf '#ifndef _BUILD_INFO_H\n#define _BUILD_INFO_H\n\n#define MARIO_BUILD_COMMIT "-g%s"\n\n#endif\n' '$(GIT_COMMIT)' > $@.tmp
	@if [ -f $@ ] && [ "$$(< $@)" = "$$(< $@.tmp)" ]; then \
		rm -f $@.tmp; \
	else \
		mv $@.tmp $@; \
	fi

clean:
	$(RM) $(OBJS)
	$(RM) $(DEPS)
	$(RM) include/build_info.h
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

%.o: %.asm
	@$(NASM) -f elf32 -o $@ $<
	@echo $@

%.o: %.c
	@$(CC) $(CFLAGS) -o $@ $<
	@echo $@
