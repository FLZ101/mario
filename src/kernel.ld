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

	/DISCARD/ : {
		*(.drectve)
		*(.rdata$zzz)
	}
}
