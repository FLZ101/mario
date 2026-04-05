RM :=rm -f

INCLUDE :=-I include
CFLAGS	:=-c $(INCLUDE) \
	-std=gnu11 -Wall -Werror \
	-ggdb -O0 -fno-omit-frame-pointer \
	-MMD -MP
AR :=ar

.PHONY: all check clean

OBJS =$(addsuffix .o,$(basename $(wildcard *.[Ssc])))
OBJS +=$(addsuffix .o,$(basename $(wildcard */*.[Ssc])))
DEPS = $(OBJS:.o=.d)

all: crt0.o libc.a | check
	cp -u $^ lib

check:
	@$(CC) --mario-verify

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
