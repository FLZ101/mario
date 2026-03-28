OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

ENTRY(_start)

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

	.init :
	{
		KEEP (*(SORT_NONE(.init)))
	}

	.text :
	{
		*(.text)
		*(.text.*)
		. = ALIGN(0x1000);
	} :text

	.fini :
	{
		KEEP (*(SORT_NONE(.fini)))
	}

	.rodata :
	{
		*(.rodata)
		*(.rodata.*)

		. = ALIGN(4);
		PROVIDE(__tm_clone_table_start = .);
		*(.tm_clone_table)
		PROVIDE(__tm_clone_table_end = .);

		. = ALIGN(0x1000);
	} :rodata

	.data :
	{
		*(.data)
		*(.data.*)
	} :data

	. = ALIGN(0x1000);
	.bss : {
		*(.bss)
		*(.bss.*)
	}

	.debug_abbrev 0 : { *(.debug_abbrev) }
	.debug_aranges 0 : { *(.debug_aranges) }
	.debug_frame 0 : { *(.debug_frame) }
	.debug_info 0 : { *(.debug_info) }
	.debug_line 0 : { *(.debug_line) }
	.debug_line_str 0 : { *(.debug_line_str) }
	.debug_rnglists 0 : { *(.debug_rnglists) }
	.debug_str 0 : { *(.debug_str) }
	.eh_frame 0 : { *(.eh_frame) }

	/*
	 * Silence warnings. The following sections should not exist in the output binary
	 */
	.rel.dyn : {
		*(.rel.got)
		*(.rel.iplt)
		*(.rel.text)
		*(.rel.text.*)
		*(.rel.rodata.*)
	}
	.got : { *(.got) }
	.got.plt : { *(.got.plt) }
	.iplt : { *(.iplt) }
	.igot.plt : { *(.igot.plt) }

	/DISCARD/ : {
		*(.comment)
		*(.note.GNU-stack)
		*(.note.gnu.property)
		*(.note.gnu.build-id)
		*(.gnu.build.attributes)
	}
}
