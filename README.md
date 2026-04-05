A simple UNIX-like OS for x86.

![](screenshot/20220122.png)

## Prerequisites

```
$ sudo dnf install make gcc binutils nasm qemu-system-x86 gdb

$ sudo dnf install wget

$ sudo dnf install socat tmux

$ sudo dnf install autoconf automake libtool
```

## Build

```
$ make
```

## Run

```
$ make quick-run
```

## Debug

```
$ make quick-debug
```

```
$ ./gdb.sh
```
