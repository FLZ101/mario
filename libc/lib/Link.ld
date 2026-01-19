OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

STARTUP(crt0.o)
ENTRY(start)
SECTIONS
{
	. = 0x00401000;
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
	}
	. = ALIGN(0x1000);
	.bss : { *(.bss) }

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
