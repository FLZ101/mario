#ifndef _PE_H
#define _PE_H

#include <types.h>

#define PE_SIGNATURE	0x00004550	/* "PE\0\0" in little endian */

/*
 * PE image file definitions;
 * Refer to "Microsoft Portable Executable and Common Object File
 * Format Specification" (Revision 8.3)
 */
struct pe_coff_hdr {
	__u16 machine_type;
	__u16 num_of_sec;
	__u32 time_stamp;
	__u32 sym_tab_off;
	__u32 num_of_sym;
	__u16 opt_hdr_sz;
	__u16 coff_flags;
};

#define PE_MACHINE_I386		0x014c
#define PE_MACHINE_AMD64	0x8664

/*
 * coff flags
 */
#define PE_RELOCS_STRIPPED	0x0001
#define PE_EXECUTABLE_IMAGE	0x0002
#define PE_AGGRESSIVE_WS_TRIM	0x0010
#define PE_32BIT_MACHINE	0x0100

struct pe_opt_hdr {
	__u16 magic;
	__u8 maj_lk_ver;
	__u8 min_lk_ver;
	__u32 text_sz;
	__u32 data_sz;
	__u32 bss_sz;
	__u32 entry;
	__u32 text_base;
	__u32 data_base;
	__u32 image_base;
	__u32 sec_align;
	__u32 file_align;
	__u16 maj_os_ver;
	__u16 min_os_ver;
	__u16 maj_img_ver;
	__u16 min_img_ver;
	__u16 maj_sub_ver;
	__u16 min_sub_ver;
	__u32 win32_ver;
	__u32 img_sz;
	__u32 hdrs_sz;
	__u32 check_sum;
	__u16 sub;
	__u16 dll_flags;
	__u32 stack_res_sz;
	__u32 stack_com_sz;
	__u32 heap_res_sz;
	__u32 heap_com_sz;
	__u32 ldr_flags;
	__u32 num_of_data_dir_ent;
};

struct pe_opt_hdr64 {
	__u16 magic;
	__u8 maj_lk_ver;
	__u8 min_lk_ver;
	__u32 text_sz;
	__u32 data_sz;
	__u32 bss_sz;
	__u32 entry;
	__u32 text_base;
	__u64 image_base;
	__u32 sec_align;
	__u32 file_align;
	__u16 maj_os_ver;
	__u16 min_os_ver;
	__u16 maj_img_ver;
	__u16 min_img_ver;
	__u16 maj_sub_ver;
	__u16 min_sub_ver;
	__u32 win32_ver;
	__u32 img_sz;
	__u32 hdrs_sz;
	__u32 check_sum;
	__u16 sub;
	__u16 dll_flags;
	__u64 stack_res_sz;
	__u64 stack_com_sz;
	__u64 heap_res_sz;
	__u64 heap_com_sz;
	__u32 ldr_flags;
	__u32 num_of_data_dir_ent;
};

#define PE32_MAGIC	0x010b
#define PE32P_MAGIC	0x020b

#define PE_DATA_DIR_ENT_SZ	8

struct pe_sec_hdr {
	char name[8];
	__u32 vir_sz;
	__u32 vir_addr;
	__u32 raw_sz;
	__u32 raw_off;
	__u32 reloc_off;
	__u32 line_off;
	__u16 num_of_reloc;
	__u16 num_of_line;
	__u32 sec_flags;
};

/*
 * section flags
 */
#define PE_SEC_TEXT	0x00000020
#define PE_SEC_DATA	0x00000040
#define PE_SEC_BSS	0x00000080
#define PE_SEC_SHARED	0x10000000
#define PE_SEC_EXEC	0x20000000
#define PE_SEC_READ	0x40000000
#define PE_SEC_WRITE	0x80000000

#endif	/* _PE_H */