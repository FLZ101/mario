A simple UNIX-like OS for x86.

![](screenshot/20220122.png)

## TODO

* TTY driver
* pipe

## How-to

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

### debug

```
$ make debug
```

```
$ i686-w64-mingw32-gdb
(gdb) target remote localhost:1234
(gdb) symbol-file kernel/kernel.dbg
(gdb) b mario
(gdb) c
```
