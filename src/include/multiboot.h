#ifndef _MULTIBOOT_H
#define _MULTIBOOT_H

/*
 * Refer to Multiboot Specification version 0.6.96
 */


#define MB_MAGIC 0x1badb002
/* all boot modules loaded must be aligned on page(4KB) boundaries */
#define	MB_FLAGS 0x00010003
#define	MB_CHECK -(MB_MAGIC + MB_FLAGS)

#define MB_MAGIC_EAX 0x2badb002

#define MB_FLAG_MEM     0x001
#define MB_FLAG_DEVICE  0x002
#define MB_FLAG_CMDLINE 0x004
#define MB_FLAG_MODULE  0x008
#define MB_FLAG_MMAP    0x040
#define MB_FLAG_LOADER  0x100
#define MB_FLAG_VBE     0x400

#ifndef __ASSEMBLY__

#include <types.h>

struct multiboot_info {
	__u32 flags;

	__u32 mem_lower;
	__u32 mem_upper;

	__u32 boot_device;

	__u32 cmdline;

	__u32 mods_count;
	__u32 mods_addr;

	__u32 ignore[4];

	__u32 mmap_length;
	__u32 mmap_addr;

	__u32 drives_length;
	__u32 drives_addr;

	__u32 config_table;

	__u32 boot_loader_name;

	__u32 apm_table;

	__u32 vbe_control_info;
	__u32 vbe_mode_info;
	__u16 vbe_mode;
	__u16 vbe_interface_seg;
	__u16 vbe_interface_off;
	__u16 vbe_interface_len;
} __attribute__((gcc_struct, packed));  /* !!! */

struct multiboot_module
{
	__u32 mod_start;
	__u32 mod_end;
	__u32 string;
	__u32 reserved;
};

#define MB_MEM_AVAILABLE 1

struct multiboot_mmap_entry {
	__u32 size;
	__u64 addr;
	__u64 len;
	__u32 type;
} __attribute__((gcc_struct, packed));  /* !!! */

#endif  /* __ASSEMBLY__ */

#endif  /* _MULTIBOOT_H */
