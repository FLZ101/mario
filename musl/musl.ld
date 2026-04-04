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
	} :text

	.text :
	{
		*(.text)
		*(.text.*)
	}

	.fini :
	{
		KEEP (*(SORT_NONE(.fini)))
		. = ALIGN(0x1000);
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

	.debug_abbrev : { *(.debug_abbrev) }
	.debug_aranges : { *(.debug_aranges) }
	.debug_frame : { *(.debug_frame) }
	.debug_info : { *(.debug_info) }
	.debug_line : { *(.debug_line) }
	.debug_line_str : { *(.debug_line_str) }
	.debug_rnglists : { *(.debug_rnglists) }
	.debug_str : { *(.debug_str) }
	.debug_loclists : { *(.debug_loclists) }
	.eh_frame : { *(.eh_frame) }
	.eh_frame_hdr : { *(.eh_frame_hdr) }

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

	.note : { *(.note.*) }

	/DISCARD/ : {
		*(.comment)
		*(.gnu.build.attributes)
	}
}
