OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

STARTUP(crt0.o)
ENTRY(start)

PHDRS
{
	headers PT_PHDR PHDRS ;
	text PT_LOAD FILEHDR PHDRS ;
	rodata PT_LOAD ;
	data PT_LOAD ;
}

SECTIONS
{
	. = 0x00401000;
	.text :
	{
		*(.text)
		. = ALIGN(0x1000);
	} :text

	.rodata :
	{
		*(.rodata)
		. = ALIGN(0x1000);
	} :rodata

	.data :
	{
		*(.data)
	} :data

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

	/*
	 * Silence warnings. The following sections should not exist in the output binary
	 */
	.rel.dyn : { *(.rel.got) *(.rel.iplt) }
	.got : { *(.got) }
	.got.plt : { *(.got.plt) }
	.iplt : { *(.iplt) }
	.igot.plt : { *(.igot.plt) }

	/DISCARD/ : {
		*(.comment)
		*(.note.GNU-stack)
		*(.note.gnu.property)
	}
}
