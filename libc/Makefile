RM :=rm -f

TOOLCHAIN :=
CC :=$(TOOLCHAIN)gcc -m32
INCLUDE :=-I include
CFLAGS	:=-c $(INCLUDE) \
	-std=gnu11 -Wall -Werror \
	-nostdinc -ffreestanding \
	-fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ggdb -O0 -fno-omit-frame-pointer -fno-stack-protector \
	-MMD -MP
AR :=$(TOOLCHAIN)ar

.PHONY: all clean

OBJS =$(addsuffix .o,$(basename $(wildcard *.[Ssc])))
OBJS +=$(addsuffix .o,$(basename $(wildcard */*.[Ssc])))
DEPS = $(OBJS:.o=.d)

all: crt0.o libc.a
	cp -u $^ lib

libc.a: $(OBJS)
	$(RM) $@
	$(AR) qc $@ $^
	$(AR) s $@

-include $(DEPS)

%.o: %.s
	@$(CC) $(CFLAGS) -o $@ $<
	@echo $@

%.o: %.c
	@$(CC) $(CFLAGS) -o $@ $<
	@echo $@

%.o: %.S
	@$(CC) $(CFLAGS) -D__ASSEMBLY__ -o $@ $<
	@echo $@

clean:
	$(RM) $(OBJS) $(DEPS) libc.a lib/libc.a lib/crt0.o
