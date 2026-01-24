OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

ENTRY(start)

PHY_BASE = 0x00100000;
VIR_BASE = 0x80100000;

/*
 * The bootloader requires the segments loaded to physical memory
 * to be both contiguous in the image file and in physical memory.
 * Refer to Multiboot Specification version 0.6.96
 */
SECTIONS
{
	. = VIR_BASE;

	_text = .;
	.text : AT(PHY_BASE)
	{
		*(.text)
		. = ALIGN(0x1000);
		*(.rodata)
		. = ALIGN(0x1000);
	}

	.data : AT(ADDR(.data) - ADDR(.text) + PHY_BASE)
	{
		*(.data)
		. = ALIGN(0x2000);
	}
	_edata = .;

	.init_task_union :  AT(ADDR(.init_task_union) - ADDR(.text) + PHY_BASE)
	{
		*(.init_task_union)
		. = ALIGN(0x1000);
	}

	_init = .;
	.init :  AT(ADDR(.init) - ADDR(.text) + PHY_BASE)
	{
		*(.tinit)
		*(.dinit)
	}
	_einit = .;

	. = ALIGN(0x1000);

	_bss = .;
	.bss :  AT(ADDR(.bss) - ADDR(.text) + PHY_BASE)
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
