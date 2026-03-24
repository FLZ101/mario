.PHONY: all clean

ifeq ($(LIBC_DIR),)
  $(error LIBC_DIR is not defined!)
endif

TOOLCHAIN :=
RM :=rm -f
CP :=cp
CC :=$(TOOLCHAIN)gcc -m32
INCLUDE :=-I$(LIBC_DIR)/../include
CFLAGS	:=-c $(INCLUDE) \
	-std=gnu11 -Wall -Werror \
	-nostdinc -ffreestanding \
	-fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ggdb -O0 -fno-omit-frame-pointer -fno-stack-protector \
	-MMD -MP
LD :=$(TOOLCHAIN)ld -m elf_i386
LDFLAGS :=-L$(LIBC_DIR) -T Link.ld --orphan-handling=warn -nostdlib -lc
OBJDUMP :=$(TOOLCHAIN)objdump

DEPS	:= $(OBJS:.o=.d)

all: $(EXE)

clean:
	$(RM) $(EXE) $(OBJS) $(DEPS)

$(EXE): $(OBJS) $(LIBC_DIR)/libc.a
	$(LD) -o $@ $^ $(LDFLAGS)

-include $(DEPS)

%.o: %.S
	@$(CC) $(CFLAGS) -D__ASSEMBLY__ -o $@ $<
	@echo $@

%.o: %.s
	@$(CC) $(CFLAGS) -o $@ $<
	@echo $@

%.o: %.c
	@$(CC) $(CFLAGS) -o $@ $<
	@echo $@
