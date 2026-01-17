CC =gcc
CFLAGS =-Wall -ggdb
SOURCES =$(wildcard *.c)
HEADERS =$(wildcard *.h)
RM =rm

.PHONY: all clean

all: rd-explorer.exe

rd-explorer.exe: $(SOURCES) $(HEADERS)
	$(CC) -o $@ $(CFLAGS) $(SOURCES)

clean:
	$(RM) -f rd-explorer.exe
