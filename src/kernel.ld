OUTPUT_FORMAT(pei-i386)

ENTRY(start)

/*
 * The bootloader requires the segments loaded to physical memory
 * to be both contiguous in the image file and in physical memory.
 * Refer to Multiboot Specification version 0.6.96
 */
SECTIONS
{
	. = 0x80000000 + 0x00100000;

	__text = .;
	.text :
	{
		*(.text)
		*(.rdata)
		*(.rdata$zzz)
		. = ALIGN(0x1000);
	}

	.data :
	{
		*(.data)
		. = ALIGN(0x2000);
	}
	__edata = .;

	.init_task_union :
	{
		*(.init_task_union)
		. = ALIGN(0x1000);
	}

	__init = .;
	.init :
	{
		*(.tinit)
		*(.dinit)
	}
	__einit = .;

	. = ALIGN(0x1000);

	__bss = .;
	.bss :
	{
		*(.bss)
	}
	__ebss = .;

	.eh_frame : {
		*(.eh_frame)
	}

	/* Base relocations — MUST be last ALLOCATED section */
	.reloc : {
		*(.reloc)
	}

	/* DWARF debug sections (GCC -g) */
	.debug_line : { *(.debug_line) }
	.debug_line_str : { *(.debug_line_str) }
	.debug_info : { *(.debug_info) }
	.debug_abbrev : { *(.debug_abbrev) }
	.debug_aranges : { *(.debug_aranges) }
	.debug_str : { *(.debug_str) }
	.debug_rnglists : { *(.debug_rnglists) }
	.debug_pubnames : { *(.debug_pubnames) }
	.debug_pubtypes : { *(.debug_pubtypes) }
	.debug_ranges : { *(.debug_ranges) }
	.debug_loc : { *(.debug_loc) }
	.debug_macinfo : { *(.debug_macinfo) }
	.debug_frame : { *(.debug_frame) }
}
