/* STICKER NFC bootloader: first 4 KB of flash, small RAM window at the start of
 * SRAM. The last SRAM word (BOOT_MAGIC_ADDR in main.c) is the DFU request from
 * the application and must stay outside every region used here. */
MEMORY
{
	FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 4K
	RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 8K
}

ENTRY(reset_handler)

SECTIONS
{
	.text :
	{
		KEEP(*(.vectors))
		*(.text*)
		*(.rodata*)
		. = ALIGN(4);
	} > FLASH

	.data :
	{
		_sdata = .;
		*(.data*)
		. = ALIGN(4);
		_edata = .;
	} > RAM AT > FLASH
	_sidata = LOADADDR(.data);

	.bss (NOLOAD) :
	{
		_sbss = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;
	} > RAM

	_estack = ORIGIN(RAM) + LENGTH(RAM);

	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
