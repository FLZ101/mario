OUTPUT_FORMAT(pei-i386)

ENTRY(start)

SECTIONS
{
	.text 0x00100000 :
	{
		__text = .;
		*(.text)
		*(.rdata)
		__etext = .;

		. = ALIGN(0x1000);
	}

	.data :
	{
		__data = .;
		*(.data)
		__edata = .;
	}

	. = ALIGN(0x1000);

	.bss :
	{
		__bss = .;
		*(.bss)
		__ebss = .;
	}

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
