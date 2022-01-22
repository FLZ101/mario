CC :=gcc
CFLAGS :=-Wall -ggdb
SOURCES :=$(wildcard *.c)
HEADERS :=$(wildcard *.h)

.PHONY: all clean test

all: rd-make.exe

rd-make.exe: $(SOURCES) $(HEADERS)
	$(CC) -o $@ $(CFLAGS) $(SOURCES)

clean:
	rm -f rd-make.exe

test:
	@./rd-make.exe --block-size=512 --device-list=../device_list \
		--dir=../ramdisk0/ --rd=../rd0
