# Why repeated `ls` was sometimes much slower

This document is a step-by-step write-up of the bug hunt. It explains how
to reproduce the symptom, how to collect evidence with GDB, what the root
causes were, and how to apply and verify the kernel fixes.

## 1. Symptom

Boot Mario with:

```sh
make quick-run
```

Enter `oksh` and press `ls` several times. Some invocations return almost
immediately, while others take much longer. The same pattern appears even
when output is discarded:

```sh
time /bin/toybox/true
time /bin/toybox/true
time /bin/toybox/ls /bin >/dev/null
time /bin/toybox/ls /bin >/dev/null
```

Typical slow timings before the fix were around 0.30-0.60s, while fast runs
were around 0.02-0.08s.

The first important clue is that even `true`, which does almost no work,
is sometimes slow. That means the problem is not in `ls` itself. It is in
process startup, scheduling, or the filesystem path used when loading the
large toybox multicall binary.

## 2. Reproduction checklist

Make sure the kernel and ramdisk are present:

```sh
make kernel
make -C qemu tmp/kernel.exe
```

Start QEMU with a serial console:

```sh
qemu-system-i386 \
  -m 256M \
  -kernel qemu/tmp/kernel.exe \
  -initrd qemu/tmp/rdx \
  -display none \
  -serial stdio \
  -no-reboot \
  -no-shutdown
```

At the `/ #` prompt, run a loop:

```sh
i=0
while [ "$i" -lt 16 ]; do time /bin/toybox/true; i=$((i+1)); done
```

Do the same for `ls`:

```sh
i=0
while [ "$i" -lt 12 ]; do time /bin/toybox/ls /bin >/dev/null; i=$((i+1)); done
```

Record the `real` values. If the times alternate between fast and slow,
the bug is reproducible.

## 3. Debug environment

The kernel has debug symbols, so GDB can attach to QEMU.

### Terminal A: QEMU with GDB stub

```sh
qemu-system-i386 \
  -s -S \
  -m 256M \
  -kernel qemu/tmp/kernel.exe \
  -initrd qemu/tmp/rdx \
  -display none \
  -serial stdio \
  -no-reboot \
  -no-shutdown
```

`-s` opens GDB on `localhost:1234`. `-S` pauses the CPU until GDB
continues.

### Terminal B: GDB

```sh
gdb -q src/kernel/kernel.exe
```

```gdb
target remote :1234
set pagination off
continue
```

Now QEMU boots. Use Terminal A to interact with the shell.

## 4. Step 1: prove the child process is slow

Set breakpoints on the entry points for process execution and exit:

```gdb
break do_exec
break do_exit
```

Add lightweight logging:

```gdb
commands 1
silent
set $cur = (struct task_struct *)((unsigned long)$esp & ~8191UL)
printf "EXEC pid=%d j=%ld counter=%d\n", $cur->pid, jiffies, $cur->counter
continue
end

commands 2
silent
set $cur = (struct task_struct *)((unsigned long)$esp & ~8191UL)
printf "EXIT pid=%d j=%ld counter=%d\n", $cur->pid, jiffies, $cur->counter
continue
end
```

Run several commands in QEMU:

```sh
time /bin/toybox/true
time /bin/toybox/true
time /bin/toybox/true
```

In GDB, compare the `j=` values for each `EXEC` and `EXIT`.

Expected observation:

```text
EXEC pid=14 j=1000
EXIT pid=14 j=1005

EXEC pid=15 j=1010
EXIT pid=15 j=1040
```

The second command spent many more jiffies between `do_exec` and `do_exit`.
This confirms the slowdown happens after the child starts executing, not
only while the shell redraws the prompt.

## 5. Step 2: find the scheduler problem

Set a breakpoint at the scheduler decision point:

```gdb
break sched.c:147
```

Log only real context switches involving the shell and its children:

```gdb
condition 3 \
  ((struct task_struct *)((unsigned long)$esp & ~8191UL))->pid >= 14 && \
  next->pid != ((struct task_struct *)((unsigned long)$esp & ~8191UL))->pid

commands 3
silent
set $cur = (struct task_struct *)((unsigned long)$esp & ~8191UL)
printf "SW cur=%d(%d) -> next=%d(%d) c=%d j=%ld state=%ld\n", \
  $cur->pid, $cur->counter, next->pid, next->counter, c, jiffies, $cur->state
continue
end
```

Run a few commands again.

Before the fix, slow commands produced repeated switches to `pid 2`:

```text
SW cur=28(0) -> next=2(10) c=10 j=9025
SW cur=28(0) -> next=2(10) c=10 j=9035
SW cur=28(0) -> next=2(10) c=10 j=9045
```

Interpretation:

- `pid 2` is the bottom-half thread.
- The PIT interrupt marks bottom-half work pending on every tick.
- `ret_common` woke `bh_thread` every time.
- That thread still had a full scheduler counter, so `schedule()` selected
  it instead of the foreground child.
- The child was repeatedly interrupted even though the bottom-half thread
  had almost no work to do.

The fix is to stop scheduling bottom halves as a normal thread. Instead,
run `do_bottom_half()` directly while returning from an interrupt or system
call.

## 6. Step 3: find the filesystem problem

Even after fixing the scheduler issue, toybox commands still did a lot of
block-I/O work. Add counters for file-system activity:

```gdb
break do_page_fault
break bread
break mario_nth_block
```

For `do_page_fault` and `bread`, only increment counters:

```gdb
commands 4
silent
set $pf = $pf + 1
continue
end

commands 5
silent
set $bread = $bread + 1
continue
end
```

For `mario_nth_block`, print the requested block number and the inode cache:

```gdb
commands 6
silent
printf "NTH ino=%lu size=%ld n=%d cache_idx=%lu cache_nr=%lu j=%ld\n", \
  inode->i_ino, inode->i_size, n, \
  inode->u.mario_i.block_index, inode->u.mario_i.block_nr, jiffies
continue
end
```

Then print counters at process exit:

```gdb
commands 2
silent
set $cur = (struct task_struct *)((unsigned long)$esp & ~8191UL)
printf "EXIT pid=%d j=%ld pf=%ld bread=%ld\n", \
  $cur->pid, jiffies, $pf, $bread
set $pf = 0
set $bread = 0
continue
end
```

Run `time /bin/toybox/true` and `time /bin/toybox/ls /bin >/dev/null`.

Expected observation before the fix:

```text
NTH ino=6380976 size=452524 n=9   cache_idx=1   cache_nr=12571
NTH ino=6380976 size=452524 n=363 cache_idx=9   cache_nr=12579
NTH ino=6380976 size=452524 n=597 cache_idx=363 cache_nr=12933
NTH ino=6380976 size=452524 n=1   cache_idx=597 cache_nr=13167
```

The inode is the toybox binary. Its page faults are not sequential; the
program touches pages all over the executable. Before the fix,
`mario_nth_block()` starts from block 1 every time it needs to go backward
or to a distant forward block. That makes loading the executable
O(n^2) in the number of blocks.

After the cache is added, `mario_nth_block()` can reuse the last known
position and, for contiguous files created by `rd-make`, can compute block
numbers directly once the contiguous prefix is known.

## 7. Step 4: find the readdir buffer bug

While inspecting the MarioFS code, `mario_readdir()` had this pattern:

```c
brelse(bh);
block = *(int *)(bh->b_data + size - 4);
```

`brelse()` releases the buffer head. The next-block pointer must be read
before the buffer is released:

```c
block = *(int *)(bh->b_data + size - 4);
brelse(bh);
```

This is a correctness fix rather than the main performance fix, but it
removes a use-after-release race.

## 8. Root causes

1. **Bottom-half scheduling**

   `bh_thread` was woken on every PIT tick and was selected by the
   scheduler as if it were ordinary foreground work.

2. **No MarioFS block cache**

   `mario_nth_block()` walked a file's singly linked block chain from the
   beginning for every block lookup. Large toybox executables made this
   very expensive.

3. **Use-after-release in `mario_readdir()`**

   The next block pointer was read after `brelse()`.

## 9. Fixes

### Bottom halves

In `src/kernel/entry.S`, replace the `bh_thread` wakeup with inline bottom
half processing:

```asm
movl bh_mask, %eax
andl bh_active, %eax
je 1f
pushfl
cli
call do_bottom_half
popfl
1:
```

In `src/kernel/timer.c`, make `run_timer_list()` preserve the caller's
interrupt flag instead of unconditionally enabling interrupts around timer
callbacks.

Remove the bottom-half thread from:

- `src/kernel/mario.c`
- `src/kernel/bh.c`

### MarioFS block cache

In `src/include/fs/mariofs/mariofs.h`, add these fields to
`struct mario_inode_info`:

```c
unsigned long block_index;
unsigned long block_nr;
unsigned long contiguous_count;
```

Add an invalidation macro:

```c
#define MARIO_INODE_INVALIDATE_BLOCK_CACHE(i) \
	do { \
		MARIO_INODE_BLOCK_INDEX(i) = 0; \
		MARIO_INODE_BLOCK_NR(i) = 0; \
		MARIO_INODE_CONTIGUOUS_COUNT(i) = 0; \
	} while (0)
```

Update `mario_nth_block()` in `src/fs/mariofs/super_block.c` to:

- use the cached block when possible,
- start from the known contiguous prefix when walking forward,
- extend the contiguous prefix when `next_block == current_block + 1`,
- remember the resulting block and index.

Invalidate the cache whenever the block chain can change:

- `src/fs/mariofs/file.c`: before `mario_file_truncate()` and
  `mario_file_write()`.
- `src/fs/mariofs/dir.c`: before `mario_add_entry()` modifies a directory.
- `mario_read_inode()` in `src/fs/mariofs/super_block.c`: reset the cache
  after reading a fresh inode.

### readdir buffer fix

In `src/fs/mariofs/dir.c`, move the next-block read before `brelse()`.

## 10. Rebuild and verify

```sh
make kernel
make -C qemu tmp/kernel.exe
```

Boot and run the same loops:

```sh
time /bin/toybox/true
time /bin/toybox/true
time /bin/toybox/ls /bin >/dev/null
time /bin/toybox/ls /bin >/dev/null
```

Check that:

- toybox `true` no longer repeatedly shows 0.30-0.60s,
- GDB no longer shows repeated foreground-child switches to `pid 2`,
- `bread` and `mario_nth_block` no longer perform repeated full-chain walks.

Before the fix, toybox `true` frequently took about 0.30-0.38s. After the
fix it was typically around 0.01-0.16s.

## 11. Files changed

- `src/kernel/entry.S`
- `src/kernel/timer.c`
- `src/kernel/mario.c`
- `src/kernel/bh.c`
- `src/include/fs/mariofs/mariofs.h`
- `src/fs/mariofs/super_block.c`
- `src/fs/mariofs/file.c`
- `src/fs/mariofs/dir.c`
