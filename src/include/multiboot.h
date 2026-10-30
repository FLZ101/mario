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
#define MB_FLAG_APM     0x400
#define MB_FLAG_VBE     0x800
#define MB_FLAG_FRAMEBUFFER 0x1000

/* Multiboot header mode types. */
#define MB_VIDEO_TYPE_GRAPHICS 0
#define MB_VIDEO_TYPE_TEXT     1

#define MB_FRAMEBUFFER_TYPE_INDEXED 0
#define MB_FRAMEBUFFER_TYPE_RGB     1
#define MB_FRAMEBUFFER_TYPE_EGA_TEXT 2

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

	__u64 framebuffer_addr;
	__u32 framebuffer_pitch;
	__u32 framebuffer_width;
	__u32 framebuffer_height;
	__u8 framebuffer_bpp;
	__u8 framebuffer_type;
	union {
		struct {
			__u32 framebuffer_palette_addr;
			__u16 framebuffer_palette_num_colors;
		} __attribute__((gcc_struct, packed));
		struct {
			__u8 framebuffer_red_field_position;
			__u8 framebuffer_red_mask_size;
			__u8 framebuffer_green_field_position;
			__u8 framebuffer_green_mask_size;
			__u8 framebuffer_blue_field_position;
			__u8 framebuffer_blue_mask_size;
		} __attribute__((gcc_struct, packed));
	};
} __attribute__((gcc_struct, packed));  /* !!! */

struct multiboot_module
{
	__u32 mod_start;
	__u32 mod_end;
	__u32 string;
	__u32 reserved;
};

struct multiboot_vbe_mode_info {
	__u16 mode_attributes;
	__u8 win_a_attributes;
	__u8 win_b_attributes;
	__u16 win_granularity;
	__u16 win_size;
	__u16 win_a_segment;
	__u16 win_b_segment;
	__u32 win_func_ptr;
	__u16 bytes_per_scanline;
	__u16 x_resolution;
	__u16 y_resolution;
	__u8 x_char_size;
	__u8 y_char_size;
	__u8 number_of_planes;
	__u8 bits_per_pixel;
	__u8 number_of_banks;
	__u8 memory_model;
	__u8 bank_size;
	__u8 number_of_image_pages;
	__u8 reserved1;
	__u8 red_mask_size;
	__u8 red_field_position;
	__u8 green_mask_size;
	__u8 green_field_position;
	__u8 blue_mask_size;
	__u8 blue_field_position;
	__u8 reserved_mask_size;
	__u8 reserved_field_position;
	__u8 direct_color_mode_info;
	__u32 phys_base_ptr;
} __attribute__((gcc_struct, packed));

#define MB_MEM_AVAILABLE 1

struct multiboot_mmap_entry {
	__u32 size;
	__u64 addr;
	__u64 len;
	__u32 type;
} __attribute__((gcc_struct, packed));  /* !!! */

#endif  /* __ASSEMBLY__ */

#endif  /* _MULTIBOOT_H */
