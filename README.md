A simple Linux clone for x86.

## Screenshots

**startup**

![](screenshot/20260504-startup.png)

**oksh**

![](screenshot/20260504-oksh.png)

**vim**

![](screenshot/20260504-vim-1.png)

![](screenshot/20260504-vim-2.png)

**nano**

![](screenshot/20260504-nano.png)

**less**

![](screenshot/20260504-less.png)

**python**

![](screenshot/20260506-python-1.png)

![](screenshot/20260506-python-2.png)

![](screenshot/20260506-python-3.png)

**cmatrix**

![](screenshot/20260507-cmatrix.png)

**2048**

![](screenshot/20260507-2048.png)

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
