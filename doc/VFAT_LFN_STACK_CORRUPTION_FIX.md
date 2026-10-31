# VFAT LFN Stack Corruption Fix

## Symptom

`tests/shell/vfat.write.test` intermittently timed out under SMP. Before the
real fix, the visible failure moved around as memory reuse changed. Adding a
guard in `free_pages()` that refused to free pages with non-zero `count`
made the timeout disappear, but that only converted the corrupted free into
an assertion/log and did not explain why a page was still referenced when it
was returned to the buddy allocator.

The actual bug was a stack out-of-bounds write in the FAT long-file-name
(LFN) lookup path. The page with non-zero `count` was the downstream victim:
`namei_at()` called `page_free()` on the wrong address after `_namei()`
returned with a corrupted saved `esi` register.

## Root Cause

`struct fat_lfn` is a stack object with a 256-byte name buffer:

```c
struct fat_lfn {
	char name[256];
	int len;
	uint8_t checksum;
	int have_last;
	int next_seq;
};
```

`fat_lfn_finish()` trusts `lfn->len`:

```c
lfn->name[lfn->len] = '\0';
```

`fat_lookup()` declared this object but, unlike `fat_dir_find_entry()` and
`fat_readdir()`, did not initialize it before scanning directory entries:

```c
struct fat_lfn lfn;
int lfn_pending;
...
lfn_pending = 0;   /* lfn itself was left uninitialized */
```

When the directory contained LFN entries, `fat_lfn_add()` used the
uninitialized `lfn->len`. A stale stack value (observed as `0x182`, or 386)
was larger than `sizeof(lfn->name)`. `fat_lfn_finish()` then wrote the
terminating NUL 130 bytes past the name buffer.

That write landed in the adjacent stack frame and overwrote `_namei()`'s
saved `esi` slot. When `_namei()` returned, `namei_at()` restored `esi` from
the corrupted slot and called:

```c
page_free(esi);
```

with the wrong virtual address. This put a live page back on the buddy free
list while its original owner still referenced it, causing the page-allocator
corruption and the nonzero `count` observation.

## Fix

Zero the LFN state in `fat_lookup()` before scanning a directory:

`src/fs/fat/dir.c:1238`

```c
fat_lfn_reset(&lfn);
```

This is the same initialization already performed by `fat_dir_find_entry()`
and `fat_readdir()`. The temporary `free_pages()` count guard was reverted.

## How to Find This With GDB

### 1. Reproduce the test

```sh
make kernel
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 1 \
    --test vfat.write.test tests/shell/vfat.write.test
```

The test uses a writable FAT32 image and eventually removes many LFN files,
which is the path that triggered the corrupted lookup.

### 2. Boot QEMU with a GDB stub

Use the same disk/device layout as `tests/lit_shell_test.py`:

```sh
qemu-system-i386 \
  -s -smp 8 -m 256M \
  -kernel src/kernel/kernel.exe \
  -initrd rd/rdx \
  -display none -serial stdio \
  -no-reboot -no-shutdown \
  -snapshot \
  -drive id=hd1,file=qemu/hd1.img,format=raw,if=none \
  -drive id=hd2,file=qemu/hd2.img,format=raw,if=none \
  -drive id=hd3,file=qemu/hd3.img,format=raw,if=none \
  -device ahci,id=ahci \
  -device ide-hd,drive=hd1,bus=ahci.0 \
  -device ide-hd,drive=hd2,bus=ahci.1 \
  -device ide-hd,drive=hd3,bus=ahci.2
```

At the serial console, set the test prompt:

```sh
PS1='MARIO_TEST_PROMPT> '
```

### 3. First clue: `tmp` survives, but `esi` does not

Attach GDB:

```sh
gdb -q src/kernel/kernel.exe
```

```gdb
target remote :1234
set pagination off
set confirm off
```

`namei_at()` stores the result of `getname()` in a local stack slot and loads
it into `esi` immediately before calling `_namei()`. In the optimized
kernel, break at the instruction just before that call:

```gdb
b *namei_at+0x82 if *(unsigned int*)($ebp+0xc) == 0x400008e9
c
```

Drive the shell through `tests/shell/vfat.write.test` until the breakpoint
hits for `/mnt/m9`. Print the local `tmp` slot and `esi`:

```gdb
p/x *(unsigned int*)($ebp-0xc)
p/x $esi
```

Observed values were the same at this point:

```text
tmp = 0x8ff63000
esi = 0x8ff63000
```

Continue to immediately after the `_namei()` call:

```gdb
b *namei_at+0x92
c
```

After `_namei()` returned, `tmp` was unchanged but `esi` had changed:

```text
tmp = 0x8ff63000
esi = 0x8f003000
```

`namei_at()` later executes `page_free(esi)`, so the wrong page was freed.
The corrupted register had to come from `_namei()`'s saved-`esi` stack slot,
not from `tmp` itself.

### 4. Watch the saved `esi` slot

Stop again at the same `/mnt/m9` breakpoint before the `_namei()` call:

```gdb
b *namei_at+0x82 if *(unsigned int*)($ebp+0xc) == 0x400008e9
c
```

At this instruction, `_namei()`'s saved `esi` slot will be `$esp - 0x20`.
Set a hardware watchpoint on that word:

```gdb
set $saved_esi_addr = $esp - 0x20
watch *(unsigned int*)$saved_esi_addr
c
```

The first watchpoint hit is expected: `_namei()`'s prologue is saving `esi`.
Continue past it:

```gdb
c
```

The second watchpoint hit is the corruption:

```text
Hardware watchpoint: *(unsigned int*)...
Old value = 0x8ff63000
New value = 0x8f003000
```

Print the backtrace and registers:

```gdb
bt 40
p/x $eip
p/x $eax
```

The observed stop was in inlined `fat_lfn_finish()` at
`fs/fat/dir.c:134`, with:

```text
eax = 0x182   # 386, used as lfn->len
eip = 0x8013552d
```

The instruction just before the watchpoint stop was:

```asm
mov BYTE PTR [ebp+eax*1-0x128], 0x0
```

That is:

```c
lfn->name[lfn->len] = '\0';
```

with `lfn->len == 386`, while `lfn->name` is only 256 bytes. The write escaped
the `fat_lookup()` frame and hit `_namei()`'s saved register slot.

### 5. Confirm the source bug

Inspect the three LFN users:

- `fat_dir_find_entry()` calls `fat_lfn_reset(&lfn);`
- `fat_readdir()` calls `fat_lfn_reset(&lfn);`
- `fat_lookup()` only set `lfn_pending = 0;`

That asymmetry is the bug.

## Verification

```sh
make kernel
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 1 \
    --test vfat.write.test tests/shell/vfat.write.test
```

After the fix, `vfat.write.test` passes repeatedly with SMP 2 and SMP 8.
The `vfat.test` and `vfat.stress.test` cases also pass.
