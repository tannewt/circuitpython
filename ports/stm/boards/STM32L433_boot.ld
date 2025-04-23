/*
    GNU linker script for STM32L433 with bootloader
*/

/* Specify the memory areas */
MEMORY
{
    FLASH (rx)          : ORIGIN = 0x08000000, LENGTH = 256k /* entire flash */
    FLASH_ISR (rx)      : ORIGIN = 0x08010000, LENGTH = 4K /* ISR vector. Kind of wasteful. */
    FLASH_FIRMWARE (rx) : ORIGIN = 0x08011000, LENGTH = 192K-64K-4K
    FLASH_FS (rw)       : ORIGIN = 0x08030000, LENGTH = 60K
    RAM (xrw)           : ORIGIN = 0x20000000, LENGTH = 640K
}


/* produce a link error if there is not this amount of RAM for these sections */
_minimum_stack_size = 24K;
_minimum_heap_size = 16K;

/* Define the top end of the stack.  The stack is full descending so begins just
   above last byte of RAM.  Note that EABI requires the stack to be 8-byte
   aligned for a call. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* RAM extents for the garbage collector */
_ram_start = ORIGIN(RAM);
_ram_end = ORIGIN(RAM) + LENGTH(RAM);
