RM :=rm -f

TOOLCHAIN :=
CC :=$(TOOLCHAIN)gcc -m32
INCLUDE :=-I include
CFLAGS	:=-c $(INCLUDE) \
	-nostdinc -ffreestanding \
	-fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ggdb -Wall -O0 -fno-omit-frame-pointer -fno-stack-protector
AR :=$(TOOLCHAIN)ar

.PHONY: all clean

OBJS =$(addsuffix .o,$(basename $(wildcard *.[Ssc])))

all: crt0.o libc.a
	cp -u $^ lib

libc.a: $(OBJS)
	$(RM) $@
	$(AR) qc $@ $^
	$(AR) s $@

%.o: %.s
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -D__ASSEMBLY__ -o $@ $<

clean:
	$(RM) $(OBJS) libc.a lib/libc.a lib/crt0.o
