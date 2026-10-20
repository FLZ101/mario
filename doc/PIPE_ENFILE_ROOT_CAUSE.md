# Why `echo hi | cat` returned `ENFILE` (root-cause notes)

## Symptom

In the Mario shell test environment, any pipeline failed like this:

```text
MARIO_TEST_PROMPT> echo hi | cat
/bin/oksh: can't create pipe - try again
```

The shell calls `pipe(2)`, the kernel returns `-ENFILE` (`23`), and
`oksh` prints its generic pipe error message.

## Starting point

`sys_pipe()` is implemented in `src/fs/pipe.c`. Its `do_pipe()` helper can
return `-ENFILE` from three obvious places:

1. `get_pipe_inode()` fails.
2. `get_two_empty_files()` fails.
3. Two free per-process fd slots cannot be found.

To find which path was taken, temporary `printk()` probes were added to
`do_pipe()` and `sys_pipe()`.

## Important debugging detail: read `dmesg`, not the console

The lit shell runner starts QEMU with:

```text
-display none
-serial stdio
```

Kernel `printk()` writes to the VGA console and the new ring buffer, but the
test runner is attached to the serial console. Therefore the temporary
`printk()` messages do **not** appear in the test transcript. They do appear
in the kernel ring buffer, so the probe test needs an in-guest `dmesg`.

Create a small probe test, for example `/tmp/pipe_probe.test`:

```text
RUN: export PATH=/bin/toybox:/bin/ubase:/bin
RUN: echo hi | cat; echo pipe-status:$?
RUN: dmesg
```

Run it with:

```bash
python3 tests/lit_shell_test.py --test pipe_probe.test /tmp/pipe_probe.test -v
```

The relevant part of the output was:

```text
<6[0.550000] DBG sys_pipe: entered
<6[0.550000] DBG do_pipe: get_two_empty_files failed
<6[0.550000] DBG sys_pipe: do_pipe=-23
```

This narrowed the failure to `get_two_empty_files()`.

## Disproving "the global file table is exhausted"

The next question was whether `get_two_empty_files()` failed because there
really were fewer than two free `struct file` objects.

A temporary helper was added to `src/fs/file.c`:

```c
int file_pool_free_count(int *samples)
{
	int i;
	int count = 0;

	ACQUIRE_LOCK(&file_lock);
	for (i = 0; i < NR_FILE; i++)
		if (!all[i].f_count)
			count++;
	RELEASE_LOCK(&file_lock);
	return count;
}
```

`do_pipe()` was temporarily changed to call it before and after
`get_two_empty_files()`:

```c
free_before = file_pool_free_count(samples);
get_two = get_two_empty_files(f);
free_after = file_pool_free_count(samples);

printk("DBG do_pipe: get_two=%d free_before=%d free_after=%d\n",
       get_two, free_before, free_after);
```

The probe test now showed:

```text
<6[0.550000] DBG do_pipe: get_two=1 free_before=987 free_after=985
```

There were 987 free `struct file` objects, and `get_two_empty_files()` had
consumed exactly two of them (`987 -> 985`). It had therefore done the
successful allocation work, but still returned `1` instead of `0`.

That is the classic signature of a clobbered function return value.

## Inspecting the generated assembly

After rebuilding, inspect the object file:

```bash
objdump -d src/fs/file.o
```

The end of `get_two_empty_files()` contained this pattern before the fix:

```asm
    xor    %eax,%eax           ; intended return value 0
    xchg   %eax,file_lock      ; RELEASE_LOCK: writes %eax with old lock value
    pop    %ebx
    pop    %esi
    pop    %ebp
    ret
```

`RELEASE_LOCK` is implemented with `xchgl`:

```c
#define RELEASE_LOCK(lock) \
do { \
	__asm__ __volatile__ ( \
		"xchgl %0, %1" \
		:"+m"((lock)->locked) \
		:"a"(0)); \
} while(0)
```

The inline assembly writes `%eax` (the old lock value, here `1`) but does not
tell GCC that `%eax` is modified. GCC still believes `%eax` contains the
previous `xor %eax,%eax` result (`0`), so the generated function returns
the old lock value.

This made a successful `get_two_empty_files()` return `1`, which `do_pipe()`
treated as failure and translated to `-ENFILE`.

## The fix

Declare `%eax` as a read/write operand so GCC knows `xchgl` modifies it.
Also mark `%eax` and `cc` correctly for `ACQUIRE_LOCK`, whose `andl`/`jnz`
sequence has the same class of missing constraints.

Updated `src/include/lib/spinlock.h`:

```c
#define ACQUIRE_LOCK(lock) \
do { \
	unsigned int __lock_value = 1; \
	__asm__ __volatile__( \
		"1:\n" \
		"xchgl %0, %1\n\t" \
		"andl %1, %1\n\t" \
		"jnz 1b" \
		:"+m"((lock)->locked), "+a"(__lock_value) \
		: \
		:"cc"); \
} while (0)

#define RELEASE_LOCK(lock) \
do { \
	unsigned int __lock_value = 0; \
	__asm__ __volatile__ ( \
		"xchgl %0, %1" \
		:"+m"((lock)->locked), "+a"(__lock_value) \
		: ); \
} while(0)
```

## Verification

Rebuild and run the same probe:

```bash
make CC=/home/fenglei/work/mario/util/mario-cc LD=/home/fenglei/work/mario/util/mario-ld -C src
python3 tests/lit_shell_test.py --test pipe_probe.test /tmp/pipe_probe.test -v
```

Expected result:

```text
MARIO_TEST_PROMPT> echo hi | cat; echo pipe-status:$?
hi
pipe-status:0
```

Focused regression tests:

```bash
python3 tests/lit_shell_test.py --test oksh-0.test tests/shell
python3 tests/lit_shell_test.py --test dmesg.test tests/shell
```

Full shell suite:

```bash
python3 tests/lit_shell_test.py tests/shell
```

All of these passed after the spinlock fix.
