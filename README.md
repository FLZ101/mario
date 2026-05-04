A simple Linux clone for x86.

## Screenshots

![](screenshot/20260504-startup.png)

![](screenshot/20260504-oksh.png)

![](screenshot/20260504-vim-1.png)

![](screenshot/20260504-vim-2.png)

![](screenshot/20260504-nano.png)

## Prerequisites

```
$ sudo dnf install make gcc binutils nasm qemu-system-x86 gdb

$ sudo dnf install wget

$ sudo dnf install autoconf automake libtool

$ sudo dnf install socat tmux
```

## Build

```
$ make
```

## Run

```
$ make quick-run
```

```
$ make quick-run-gtk
```

## Debug

```
$ make quick-debug
```

```
$ make quick-debug-gtk
```

```
$ ./gdb.sh
```
