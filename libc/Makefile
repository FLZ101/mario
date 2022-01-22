RM :=rm -f

TOOLCHAIN :=i686-w64-mingw32-
CC :=$(TOOLCHAIN)gcc -m32
INCLUDE :=-I include
CFLAGS :=-c -nostdinc -ffreestanding $(INCLUDE) -ggdb
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
