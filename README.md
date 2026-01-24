A simple UNIX-like OS for x86.

![](screenshot/20220122.png)

## TODO

* TTY driver
* pipe

## How-to

### prerequisites

```
$ sudo dnf install make gcc binutils nasm qemu-system-x86
```

### build the kernel (and the bootloader)

```
$ make kernel
```

### prepare the root filesystem

```
$ make rd
```

### create the harddisk image used by qemu

```
$ make image
```

### run

```
$ make run
```

```
$ make quick-run
```

### debug

```
$ make debug
```

```
$ make quick-debug
```

```
$ ./gdb.sh
```
