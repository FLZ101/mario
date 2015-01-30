OUTPUT_FORMAT(pei-i386)

ENTRY(start)

SECTIONS
{
	. = 0x00100000;

	__text = .;
	.text :
	{
		*(.multiboot)
		*(.text)
		*(.rdata)
		. = ALIGN(0x1000);
	}
	__etext = .;

	__data = .;
	.data :
	{
		
		*(.data)
	}
	__edata = .;

	. = ALIGN(0x1000);

	__bss = .;
	.bss :
	{
		*(.bss)
	}
	__ebss = .;

	.eh_frame :
	{
		*(.eh_frame)
	}

	/DISCARD/ :
	{
		*(.rdata$zzz)
		*(.drectve)
	}
}
