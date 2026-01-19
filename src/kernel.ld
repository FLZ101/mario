OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

ENTRY(start)

/*
 * The bootloader requires the segments loaded to physical memory
 * to be both contiguous in the image file and in physical memory.
 * Refer to Multiboot Specification version 0.6.96
 */
SECTIONS
{
	. = 0x80000000 + 0x00100000;

	_text = .;
	.text :
	{
		*(.text)
		. = ALIGN(0x1000);
		*(.rodata)
		. = ALIGN(0x1000);
	}

	.data :
	{
		*(.data)
		. = ALIGN(0x2000);
	}
	_edata = .;

	.init_task_union :
	{
		*(.init_task_union)
		. = ALIGN(0x1000);
	}

	_init = .;
	.init :
	{
		*(.tinit)
		*(.dinit)
	}
	_einit = .;

	. = ALIGN(0x1000);

	_bss = .;
	.bss :
	{
		*(.bss)
	}
	_ebss = .;

	.debug_abbrev 0 : { *(.debug_abbrev) }
	.debug_aranges 0 : { *(.debug_aranges) }
	.debug_frame 0 : { *(.debug_frame) }
	.debug_info 0 : { *(.debug_info) }
	.debug_line 0 : { *(.debug_line) }
	.debug_line_str 0 : { *(.debug_line_str) }
	.debug_rnglists 0 : { *(.debug_rnglists) }
	.debug_str 0 : { *(.debug_str) }

	/DISCARD/ : {
		*(.comment)
		*(.note.GNU-stack)
		*(.note.gnu.property)
	}
}
