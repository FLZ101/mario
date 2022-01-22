OUTPUT_FORMAT(pei-i386)

STARTUP(crt0.o)
ENTRY(start)
SECTIONS
{
	. = 0x00401000;
	.text :
	{
		*(.text)
		*(.rdata)
		. = ALIGN(0x1000);
	}
	.data :
	{
		*(.data)
	}
	. = ALIGN(0x1000);
	.bss : { *(.bss) }

	.eh_frame : { *(.eh_frame) }

	/DISCARD/ : { *(.rdata$zzz) *(.drectve) *(.xdata) *(.pdata)}
}
